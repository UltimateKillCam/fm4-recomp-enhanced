"""Read-only overlay planner, NOT an installer. No source file is written.

Only actual documented extracted-tree layouts are recognized. Publishing,
runtime readiness and harder-AI variants intentionally remain unavailable.
"""
from pathlib import Path
import os,stat
from resource_codec import Invalid,relative

MODES={'normal':'Normal Mode','hard':'Hard Mode','sandbox':'Sandbox Mode','vanilla':'Vanilla Mode'}
REPARSE=0x400

def safe_absolute(path,edition_root):
    value=Path(path)
    if not value.is_absolute():value=Path(edition_root)/value
    value=Path(os.path.abspath(value))
    # Inspect BEFORE resolve: resolving first would hide a junction/symlink.
    for part in [*reversed(value.parents),value]:
        try:info=part.lstat()
        except FileNotFoundError:continue
        if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&REPARSE:
            raise Invalid('Reparse point in selected path: '+str(part))
    return value

def overlap(a,b):
    a=os.path.normcase(os.path.abspath(a));b=os.path.normcase(os.path.abspath(b))
    try:return os.path.commonpath([a,b]) in (a,b)
    except ValueError:return False

def destination(edition_root,path,stock,source,update=None):
    edition_root=safe_absolute(edition_root,edition_root)
    target=safe_absolute(path or '.local/forza-plus/assets',edition_root)
    protected=[stock,source,edition_root/'.local/user']
    if update:protected.append(update)
    for item in protected:
        item=safe_absolute(item,edition_root)
        if overlap(target,item):raise Invalid('Prepared destination overlaps stock/source/update/user: '+str(item))
    if target.exists() and not target.is_dir():raise Invalid('Prepared destination is not a directory')
    return target

def files(root):
    root=safe_absolute(root,root)
    if not root.is_dir():raise Invalid('Missing source directory: '+str(root))
    for directory,dirs,names in os.walk(root,followlinks=False):
        for name in dirs+names:
            path=Path(directory)/name
            if path.is_symlink() or getattr(path.lstat(),'st_file_attributes',0)&REPARSE:
                raise Invalid('Reparse point in source tree: '+str(path))
        for name in sorted(names):yield Path(directory)/name

def map_files(root):
    result={}
    for path in files(root):
        key=path.relative_to(root).as_posix().casefold()
        if key in result:raise Invalid('Case-colliding source files: '+key)
        result[key]=path
    return result

def game_overlays(source,mode,update=None):
    if mode not in MODES:raise Invalid('Unrecognized career mode')
    source=Path(source);result={};preserved=[]
    def layer(root,label,required):
        media=root/'Main Game Changes/Media'
        database=root/'Game Mode DBs'/MODES[mode]/'gamedb.slt'
        if required and (not media.is_dir() or not database.is_file()):
            raise Invalid('Expected base mod Media and direct selected-mode gamedb.slt')
        if media.is_dir():
            for key,path in map_files(media).items():
                name='media/'+key
                if key=='renderscenarios' or key.startswith('renderscenarios/'):
                    preserved.append(name);continue
                if path.suffix.casefold()=='.xex':raise Invalid('Unexpected executable under mod Media')
                result[name]={'path':path,'layer':label}
        if database.is_file():result['media/db/gamedb.slt']={'path':database,'layer':label+' selected mode'}
        return database.is_file()
    layer(source,'base',True)
    if update:
        # Do not silently reinterpret arbitrary updates or fall back to another mode.
        if not layer(Path(update),'update',True):raise Invalid('Missing selected mode in update')
    return result,sorted(set(preserved))

def resolve_protected(entries,stock,overlays):
    stock_map=map_files(Path(stock))
    result={}
    for entry in entries:
        key=str(relative(entry.name)).casefold()
        path=overlays[key]['path'] if key in overlays else stock_map.get(key)
        if path is None:raise Invalid('Missing protected file: '+key)
        result[key]=path
    return result

def content_packages(source,mode,stock_user,*,allow_headerless_local_mod_packages=False):
    """Plan only installed packages having genuine existing runtime headers."""
    source=Path(source);root=source/'Disc 2 and DLCs/00000002'
    installed=Path(stock_user)/'0000000000000000/4D530910'
    mode_root=source/'Game Mode DBs'/MODES[mode]/'DLC DBs'
    result=[]
    if not root.is_dir():raise Invalid('Missing base content directory')
    for package in sorted(root.iterdir()):
        safe_absolute(package,source)
        if not package.is_dir():continue
        current=installed/'00000002'/package.name
        header=installed/'Headers/00000002'/(package.name+'.header')
        valid=current.is_dir() and header.is_file()
        local=not valid and allow_headerless_local_mod_packages
        result.append({'package':package.name,'eligible':valid or local,
            'registration_kind':'existing_native_header' if valid else ('local_mod_directory' if local else 'omitted'),
            'package_license_mask':0 if local else None,'native_acceptance_validated':False,
            'reason':None if valid else ('runtime directory fallback; zero package license; FM4 acceptance unproved' if local else 'missing installed content or genuine runtime header'),
            'source':str(package),'stock_content':str(current),'stock_header':str(header),
            'mode_overlay':str(mode_root/package.name) if (mode_root/package.name).is_dir() else None})
    return result
