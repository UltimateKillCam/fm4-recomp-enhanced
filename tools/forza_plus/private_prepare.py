"""CPU-only immutable asset preparation and full receipt verifier. Modified 2026-10-23.

No implicit CLI/run/install. Callers must hold an edition installation lock and
exclude a running game before publication. Preparation does not launch the game.
"""
from pathlib import Path
import json,os,shutil,uuid
from resource_codec import *
from overlay_plan import *
from prepared_database import create_prepared_copy,digest
from publication import manifest,matches,atomic_json,PROTOCOL as PUBLICATION_PROTOCOL
from edition_gate import exclusive,require_no_pending

NATIVE_XEX='b34dfb88978819cccda6dafd755e7139970a58fbf6707dfbcba06846857f52bd'
ORIGINAL_RESOURCE='9948f4053e2ebf91b3c998cb5dd49d5658f50cb9631a354ea86e2d4c5a512c98'

def copy_tree(source,target):
    source=Path(source);target=Path(target)
    target.mkdir(parents=True,exist_ok=True)
    for key,path in map_files(source).items():
        output=target/path.relative_to(source);output.parent.mkdir(parents=True,exist_ok=True)
        before=digest(path);shutil.copyfile(path,output)
        if digest(output)!=before or digest(path)!=before:raise Invalid('Input changed during copy: '+key)

def normalize(settings):
    if settings.get('enabled') is not True:
        if settings.get('enabled') is False:return {'enabled':False}
        raise Invalid('Enabled must be Boolean')
    mode=settings.get('mode','normal')
    if mode not in MODES:raise Invalid('Unknown career mode')
    preset=settings.get('physics_preset','stock')
    if preset not in ('stock','100','120','144','165','custom'):raise Invalid('Unknown physics preset')
    if settings.get('harder_ai_file'):raise Invalid('unsupported_harder_ai_variant')
    # All of the mod's DLC packages by default (2026-10-23); the runtime mounts headerless folders.
    loose=settings.get('allow_headerless_local_mod_packages',True)
    if not isinstance(loose,bool):raise Invalid('Headerless local-mod option must be Boolean')
    camera=settings.get('camera_fov',{})
    if not isinstance(camera,dict) or set(camera)-set(FOV_KEYS):raise Invalid('Unknown camera settings')
    # Scalar validation uses a disposable text fixture; no original file edit.
    test=b'\r\n'.join(k+b' 54' for k in FOV_KEYS.values())+b'\r\n'
    camera_override(test,camera)
    target=settings.get('physics_target_fps') if preset=='custom' else None
    if preset=='custom':custom_physics(b'CarPhysicsUpdateHz 359\r\n',target)
    return {'enabled':True,'mode':mode,'physics_preset':preset,'physics_target_fps':target,
        'camera_fov':{name:camera.get(name) for name in FOV_KEYS},'harder_ai_file':None,
        'source_folder':settings.get('source_folder'),'update_folder':settings.get('update_folder'),
        'allow_headerless_local_mod_packages':loose,
        'prepared_game_folder':settings.get('prepared_game_folder') or '.local/forza-plus/assets'}

def _prepare_private(edition_root,settings,original_resource=None,*,stock_root=None,stock_user=None):
    edition=safe_absolute(edition_root,Path.cwd());options=normalize(settings)
    stock=Path(stock_root) if stock_root else edition/'.local/game-disc1'
    stock_user=Path(stock_user) if stock_user else edition/'.local/user'
    if not options['enabled']:
        return {'status':'disabled','launch_allowed':False,'game_data_root':str(stock),'user_data_root':str(stock_user)}
    if not options['source_folder']:raise Invalid('source_missing')
    source=safe_absolute(options['source_folder'],edition)
    update=safe_absolute(options['update_folder'],edition) if options['update_folder'] else None
    store=destination(edition,options['prepared_game_folder'],stock,source,update)
    options.update(source_folder=str(source),update_folder=str(update) if update else None,prepared_game_folder=str(store))
    if store.exists() and any(store.iterdir()) and not (store/'.fm4-plus-store.json').is_file():raise Invalid('Unowned prepared destination')
    store.mkdir(parents=True,exist_ok=True)
    identity={'protocol':'fm4-plus-store/1','edition_root':str(edition),'store':str(store)}
    marker=store/'.fm4-plus-store.json'
    if marker.is_file() and json.loads(marker.read_text())!=identity:raise Invalid('Prepared destination owner differs')
    if not marker.exists():atomic_json(marker,identity)
    if original_resource is None:
        from resource_source import discover
        original_resource=discover(edition,stock)
    original_resource=Path(original_resource)
    if digest(original_resource)!=ORIGINAL_RESOURCE:raise Invalid('Original native protected resource differs')
    raw=original_resource.read_bytes();entries=parse_table(raw)
    if digest(stock/'default.xex')!=NATIVE_XEX:raise Invalid('Original native executable differs')
    overlays,preserved=game_overlays(source,options['mode'],update)
    overlay_provenance={name:{'source':str(item['path']),'layer':item['layer'],'size':item['path'].stat().st_size,'sha256':digest(item['path'])} for name,item in overlays.items()}
    author_docs={}
    for label,root in [('base',source),('update',update)]:
        if not root:continue
        documents=root/'Install Instructions and Other Info'
        if documents.is_dir():author_docs[label]={name:digest(path) for name,path in map_files(documents).items()}
    generation=uuid.uuid4().hex;partial=store/'versions'/('.partial-'+generation)
    if partial.exists():raise Invalid('Private generation collision')
    game=partial/'game';game.mkdir(parents=True)
    atomic_json(partial/'owner.json',{'protocol':'fm4-plus-generation/1','generation':generation,'store':str(store)})
    # All copies live under a new private generation. A failure retains its
    # owner/diagnostic for review; no incomplete generation is selected.
    copy_tree(stock,game)
    for name,item in overlays.items():
        if name=='media/db/gamedb.slt':continue
        output=game.joinpath(*name.split('/'));output.parent.mkdir(parents=True,exist_ok=True)
        before=digest(item['path']);shutil.copyfile(item['path'],output)
        if digest(output)!=before or digest(item['path'])!=before:raise Invalid('Mod source changed during overlay')
    db_source=overlays['media/db/gamedb.slt']['path'];db_temp=partial/'prepared-native-gamedb.slt'
    database=create_prepared_copy(db_source,db_temp,stock/'Media/db/gamedb.slt')
    os.replace(db_temp,game/'Media/db/gamedb.slt')
    physics=game/'Media/physics/PhysicsSettings.ini'
    preset=options['physics_preset'];physics_report={'preset':preset,'target_fps':None,'car_physics_update_hz':None}
    if preset=='custom':
        data,hz=custom_physics(physics.read_bytes(),options['physics_target_fps']);physics.write_bytes(data)
        physics_report.update(target_fps=options['physics_target_fps'],car_physics_update_hz=hz)
    elif preset!='stock':
        selected=None
        for root in [update,source]:
            if root:
                candidate=root/'Main Game Changes/PhysicsSettings.ini for higher FPS'/(preset+'fps')/'PhysicsSettings.ini'
                if candidate.is_file():selected=candidate;break
        if selected is None:raise Invalid('Missing supplied physics preset')
        before=digest(selected);shutil.copyfile(selected,physics)
        if digest(physics)!=before or digest(selected)!=before:raise Invalid('Physics preset source changed')
        physics_report['source']=str(selected)
    camera=game/'Media/Camera/CameraSettings.ini'
    if any(value is not None for value in options['camera_fov'].values()):
        camera.write_bytes(camera_override(camera.read_bytes(),options['camera_fov']))
    packages=content_packages(source,options['mode'],stock_user,allow_headerless_local_mod_packages=options['allow_headerless_local_mod_packages']);units=[]
    content=partial/'content';content.mkdir()
    for package in packages:
        if not package['eligible']:continue
        relative=Path('0000000000000000/4D530910/00000002')/package['package']
        output=content/relative
        if package['registration_kind']=='existing_native_header':copy_tree(package['stock_content'],output)
        else:output.mkdir(parents=True,exist_ok=True)
        for layer in [source,update]:
            if not layer:continue
            common=layer/'Disc 2 and DLCs/00000002'/package['package']
            mode=layer/'Game Mode DBs'/MODES[options['mode']]/'DLC DBs'/package['package']
            if common.is_dir():copy_tree(common,output)
            if mode.is_dir():copy_tree(mode,output)
        units.append(relative.as_posix())
        if package['registration_kind']=='existing_native_header':
            header_relative=Path('0000000000000000/4D530910/Headers/00000002')/(package['package']+'.header')
            header=content/header_relative;header.parent.mkdir(parents=True,exist_ok=True)
            before=digest(package['stock_header']);shutil.copyfile(package['stock_header'],header)
            if digest(header)!=before or digest(package['stock_header'])!=before:raise Invalid('Genuine runtime header changed')
            package['runtime_header_sha256']=before;units.append(header_relative.as_posix())
        else:package['runtime_header_sha256']=None
    game_map=map_files(game)
    blob,rebuilt=rebuild(entries,lambda path:game_map[str(path).casefold()])
    (partial/'protected-resource.bin').write_bytes(blob)
    # Final DB hash includes approved compatibility changes before signatures.
    receipt={'schema':'fm4-plus-receipt/1','generation_id':generation,'edition_root':str(edition),'store':str(store),
        'options':options,'native_xex_sha256':NATIVE_XEX,'native_original_resource_sha256':ORIGINAL_RESOURCE,
        'database_adaptation':database,'physics':physics_report,'camera_fov':options['camera_fov'],
        'source_provenance':{'overlays':overlay_provenance,'author_documents':author_docs,
            'mod_xex_sha256':digest(source/'Main Game Changes/default.xex') if (source/'Main Game Changes/default.xex').is_file() else None,
            'mod_xex_policy':'never copied; native original executable and generated modules retained'},
        'game_manifest':manifest(game),'content_manifest':manifest(content),'content_units':units,
        'packages':packages,'renderer_paths_preserved':preserved,
        'protected_resource':{'entries':66,'block_size':BLOCK,'size':len(blob),'sha256':hashlib.sha256(blob).hexdigest()},
        'native_hook_available':False,'native_compatibility_validated':False,'launch_allowed':False}
    atomic_json(partial/'receipt.json',receipt)
    for name,item in overlays.items():
        if digest(item['path'])!=overlay_provenance[name]['sha256']:raise Invalid('Source changed before receipt publication')
    # Verify the full generation before its one-directory publication. This
    # does not publish content/user state or the active selection.
    verify_generation(partial,allow_partial=True)
    final=store/'versions'/generation;os.replace(partial,final)
    return {'status':'prepared_unverified','generation':str(final),'receipt_id':generation,
        'game_data_root':str(final/'game'),'user_data_root':str(edition/'.local/user/.fm4-plus'/options['mode']),
        'native_hook_available':False,'launch_allowed':False,'omitted_packages':[x['package'] for x in packages if not x['eligible']]}

def verify_generation(generation,*,allow_partial=False,quick=False):
    # quick: file set and sizes only. Launch relies on the native pre-guest full hash check;
    # preparation and publication always use the full hash verification.
    generation=safe_absolute(generation,Path.cwd());receipt=json.loads((generation/'receipt.json').read_text())
    if receipt.get('schema')!='fm4-plus-receipt/1':raise Invalid('Wrong prepared receipt schema')
    owner=json.loads((generation/'owner.json').read_text())
    if owner.get('protocol')!='fm4-plus-generation/1' or owner.get('generation')!=receipt.get('generation_id') or owner.get('store')!=receipt.get('store'):
        raise Invalid('Prepared generation ownership mismatch')
    store=safe_absolute(receipt['store'],Path.cwd());identity=receipt['generation_id']
    if not isinstance(identity,str) or len(identity)!=32 or any(x not in '0123456789abcdef' for x in identity):raise Invalid('Invalid generation identity')
    permitted=[store/'versions'/identity]
    if allow_partial:permitted.append(store/'versions'/('.partial-'+identity))
    if generation not in permitted:raise Invalid('Prepared generation path differs from its owner')
    if not matches(generation/'game',receipt['game_manifest'],quick) or not matches(generation/'content',receipt['content_manifest'],quick):
        raise Invalid('Prepared asset manifest mismatch')
    if quick:return receipt
    if digest(generation/'game/default.xex')!=NATIVE_XEX:raise Invalid('Prepared native executable changed')
    resource=(generation/'protected-resource.bin').read_bytes()
    if hashlib.sha256(resource).hexdigest()!=receipt['protected_resource']['sha256'] or len(resource)!=receipt['protected_resource']['size']:
        raise Invalid('Prepared resource receipt differs')
    entries=parse_table(resource);game_map=map_files(generation/'game')
    current,_=rebuild(entries,lambda path:game_map[str(path).casefold()])
    if current!=resource:raise Invalid('Protected resource no longer matches final assets')
    return receipt

def prepare_private(edition_root,settings,original_resource=None,*,stock_root=None,stock_user=None):
    if settings.get('enabled') is False:
        return _prepare_private(edition_root,settings,original_resource,stock_root=stock_root,stock_user=stock_user)
    with exclusive(edition_root):
        require_no_pending(edition_root)
        return _prepare_private(edition_root,settings,original_resource,stock_root=stock_root,stock_user=stock_user)
