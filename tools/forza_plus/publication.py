"""Private receipt-owned marketplace publication journal, never profiles/saves.

One atomic active receipt commits already-verified game/content generations.
Multiple package renames are explicitly NOT a globally atomic filesystem move.
"""
from pathlib import Path
import hashlib,json,os,re,shutil,uuid
from contextlib import contextmanager
from edition_gate import exclusive,require_no_pending
from overlay_plan import Invalid,safe_absolute,files

PROTOCOL='fm4-plus-publication/1'
OWNER='.fm4-plus-content.json'
PENDING='.fm4-plus-pending.json'

def blob(value):return (json.dumps(value,sort_keys=True,separators=(',',':'))+'\n').encode()
def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as source:
        while part:=source.read(1<<20):h.update(part)
    return h.hexdigest()
def manifest(path):
    path=safe_absolute(path,Path.cwd())
    if not path.exists():return None
    if path.is_file():return {'kind':'file','size':path.stat().st_size,'sha256':sha(path)}
    if not path.is_dir():raise Invalid('Unsupported publication object')
    return {'kind':'directory','files':{p.relative_to(path).as_posix():{'size':p.stat().st_size,'sha256':sha(p)} for p in sorted(files(path))}}
def matches(path,wanted,quick=False):
    """Compare an object with its receipt description; quick checks the file set and sizes only."""
    if not quick:return manifest(path)==wanted
    path=safe_absolute(path,Path.cwd())
    if wanted is None or not path.exists():return wanted is None and not path.exists()
    if wanted.get('kind')=='file':return path.is_file() and path.stat().st_size==wanted.get('size')
    if not path.is_dir():return False
    found={p.relative_to(path).as_posix():p.stat().st_size for p in files(path)}
    return found=={name:entry.get('size') for name,entry in wanted.get('files',{}).items()}
def read_json(path):return json.loads(Path(path).read_text()) if Path(path).is_file() else None
def atomic_json(path,value):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_name(path.name+'.tmp-'+uuid.uuid4().hex)
    with temporary.open('xb') as output:output.write(blob(value));output.flush();os.fsync(output.fileno())
    os.replace(temporary,path)
def unit_path(name):
    parts=name.replace('\\','/').split('/')
    prefix=['0000000000000000','4D530910']
    if parts[:2]!=prefix:raise Invalid('Publication outside isolated FM4 common content')
    if len(parts)==4 and parts[2]=='00000002':package=parts[3]
    elif len(parts)==5 and parts[2:4]==['Headers','00000002'] and parts[4].endswith('.header'):package=parts[4][:-7]
    else:raise Invalid('Publication unit must be a whole marketplace package or genuine header')
    if not re.fullmatch(r'(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{42}|4[dD]5309100000000[1-4])',package):raise Invalid('Unrecognized FM4 package identity')
    return Path(*parts)
def checked_roots(store,user):
    store=safe_absolute(store,Path.cwd());user=safe_absolute(user,Path.cwd())
    # Caller must separately validate the selected edition/source overlaps.
    if '.fm4-plus' not in user.parts:raise Invalid('Publisher requires an isolated per-mode user root')
    return store,user
def check_owner(owner,store,user):
    if owner is not None and (owner.get('protocol')!=PROTOCOL or owner.get('store')!=str(store) or owner.get('user')!=str(user)):
        raise Invalid('Unowned existing content marker')

def _publish(store,user,generation,unit_names,fault=lambda _phase:None):
    store,user=checked_roots(store,user)
    if not re.fullmatch('[0-9a-f]{32}',generation):raise Invalid('Invalid generation identity')
    game=store/'versions'/generation/'game';content=store/'versions'/generation/'content'
    safe_absolute(game,store);safe_absolute(content,store)
    if not game.is_dir():raise Invalid('Missing verified generation game')
    if (user/PENDING).exists():raise Invalid('Pending content recovery required')
    old_owner=read_json(user/OWNER);check_owner(old_owner,store,user)
    old_units=old_owner.get('units',{}) if old_owner else {}
    names=sorted(set(unit_names))
    if set(old_units)-set(names):raise Invalid('Removing previously owned packages requires a separate reviewed operation')
    operations=[];new_units={}
    stage=user/'.fm4-plus-stage'/generation;backup=user/'.fm4-plus-backup'/generation
    if stage.exists() or backup.exists():raise Invalid('Existing transaction namespace requires recovery')
    for index,name in enumerate(names):
        relative=unit_path(name);source=content/relative;target=user/relative
        safe_absolute(source,store);safe_absolute(target,user)
        new=manifest(source);old=manifest(target)
        if new is None:raise Invalid('Missing staged content unit')
        if old is not None and (name not in old_units or old!=old_units[name]):raise Invalid('Existing content unit is not receipt-owned and unchanged')
        new_units[name]=new
        if old==new:continue
        pending=stage/str(index);pending.parent.mkdir(parents=True,exist_ok=True)
        if source.is_dir():shutil.copytree(source,pending)
        else:shutil.copyfile(source,pending)
        if manifest(pending)!=new:raise Invalid('Content staging changed during copy')
        operations.append({'name':name,'index':index,'old':old,'new':new})
    journal={'protocol':PROTOCOL,'store':str(store),'user':str(user),'generation':generation,
        'old_owner':old_owner,'operations':operations,'new_units':new_units}
    user.mkdir(parents=True,exist_ok=True);atomic_json(user/PENDING,journal)
    fault('journal_written')
    for operation in operations:
        target=user/unit_path(operation['name']);pending=stage/str(operation['index']);saved=backup/str(operation['index'])
        target.parent.mkdir(parents=True,exist_ok=True)
        if operation['old'] is not None:
            saved.parent.mkdir(parents=True,exist_ok=True);os.replace(target,saved)
        fault('old_backed_up')
        os.replace(pending,target);fault('new_unit_published')
    owner={'protocol':PROTOCOL,'store':str(store),'user':str(user),'generation':generation,'units':new_units}
    atomic_json(user/OWNER,owner);fault('owner_published')
    # Commit point: after this replace, recovery validates and finishes the new
    # generation. Before it, recovery restores all previous owned content.
    atomic_json(store/'active.json',{'protocol':PROTOCOL,'generation':generation,'user':str(user)})
    fault('active_published')
    (user/PENDING).unlink()
    return owner

def _recover(store,user):
    store,user=checked_roots(store,user);journal=read_json(user/PENDING)
    if journal is None:return 'no_pending_transaction'
    check_owner(journal,store,user);generation=journal.get('generation','')
    if not re.fullmatch('[0-9a-f]{32}',generation):raise Invalid('Invalid pending generation')
    stage=user/'.fm4-plus-stage'/generation;backup=user/'.fm4-plus-backup'/generation
    safe_absolute(stage,user);safe_absolute(backup,user)
    indexes=set()
    for operation in journal['operations']:
        index=operation.get('index')
        if isinstance(index,bool) or not isinstance(index,int) or not 0<=index<8192 or index in indexes:
            raise Invalid('Invalid/duplicate pending operation identity')
        indexes.add(index);unit_path(operation['name'])
    active=read_json(store/'active.json')
    committed=active is not None and active.get('protocol')==PROTOCOL and active.get('generation')==generation and active.get('user')==str(user)
    if committed:
        from private_prepare import verify_generation
        verified=verify_generation(store/'versions'/generation)
        if verified.get('edition_root')!=str(edition_for_store(store,None)) or sorted(verified['content_units'])!=sorted(journal['new_units']):
            raise Invalid('Committed generation differs from pending receipt')
        for name,wanted in journal['new_units'].items():
            if manifest(user/unit_path(name))!=wanted:raise Invalid('Committed content changed; refuse guessed recovery')
        owner=read_json(user/OWNER);check_owner(owner,store,user)
        if owner is None or owner.get('generation')!=generation or owner.get('units')!=journal['new_units']:raise Invalid('Committed owner receipt mismatch')
        (user/PENDING).unlink();return 'finished_committed_generation'
    for operation in reversed(journal['operations']):
        target=user/unit_path(operation['name']);saved=backup/str(operation['index']);pending=stage/str(operation['index'])
        current=manifest(target)
        if current==operation['old'] and not saved.exists():continue
        if current is not None:
            if current!=operation['new']:raise Invalid('Unowned/changed target blocks recovery')
            if pending.exists():raise Invalid('Ambiguous duplicate transaction target')
            pending.parent.mkdir(parents=True,exist_ok=True);os.replace(target,pending)
        if saved.exists():
            if manifest(saved)!=operation['old']:raise Invalid('Backup changed; refuse recovery')
            target.parent.mkdir(parents=True,exist_ok=True);os.replace(saved,target)
        elif operation['old'] is not None:raise Invalid('Missing previous content backup')
    current_owner=read_json(user/OWNER);check_owner(current_owner,store,user)
    if journal['old_owner'] is not None:atomic_json(user/OWNER,journal['old_owner'])
    elif current_owner is not None:
        if current_owner.get('generation')!=generation:raise Invalid('Unknown content owner blocks recovery')
        (user/OWNER).unlink()
    (user/PENDING).unlink()
    # Private transaction stage/backups remain quarantined with the immutable
    # generation. Never recursively remove a root that may contain user data.
    return 'rolled_back_uncommitted_generation'

@contextmanager
def exclusive_user_lock(user):
    user=safe_absolute(user,Path.cwd());user.mkdir(parents=True,exist_ok=True)
    path=user/'.fm4-plus-publication.lock'
    safe_absolute(path,user)
    with path.open('a+b') as stream:
        stream.seek(0,os.SEEK_END)
        if stream.tell()==0:stream.write(b'\0');stream.flush()
        stream.seek(0)
        if os.name=='nt':
            import msvcrt
            msvcrt.locking(stream.fileno(),msvcrt.LK_NBLCK,1)
            try:yield
            finally:stream.seek(0);msvcrt.locking(stream.fileno(),msvcrt.LK_UNLCK,1)
        else:
            import fcntl
            fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
            try:yield
            finally:fcntl.flock(stream,fcntl.LOCK_UN)

def edition_for_store(store,explicit):
    owner=read_json(Path(store)/'.fm4-plus-store.json')
    if owner is None or owner.get('protocol')!='fm4-plus-store/1' or owner.get('store')!=str(Path(store)):
        raise Invalid('Edition identity required for publisher exclusion')
    edition=safe_absolute(owner['edition_root'],Path.cwd())
    if explicit and safe_absolute(explicit,Path.cwd())!=edition:raise Invalid('Requested edition differs from prepared store owner')
    return edition

def require_edition_user(edition,user):
    if user.parent!=Path(edition)/'.local/user/.fm4-plus' or user.name not in ['normal','hard','sandbox','vanilla']:
        raise Invalid('Publisher user root does not belong to the selected edition/mode')

def publish(store,user,generation,unit_names,fault=lambda _phase:None,*,edition_root=None):
    store,user=checked_roots(store,user)
    edition=edition_for_store(store,edition_root)
    require_edition_user(edition,user)
    with exclusive(edition):
        require_no_pending(edition)
        # This is the public mutation boundary. Verify here, while holding the
        # exact edition lease, rather than trust any caller-supplied unit list.
        from private_prepare import verify_generation
        receipt=verify_generation(store/'versions'/generation)
        if receipt.get('edition_root')!=str(edition) or receipt.get('store')!=str(store) or receipt.get('generation_id')!=generation:
            raise Invalid('Verified generation belongs to a different edition/store')
        verified_units=receipt['content_units']
        if sorted(unit_names)!=sorted(verified_units) or len(unit_names)!=len(set(unit_names)):
            raise Invalid('Requested content units differ from verified generation receipt')
        with exclusive_user_lock(user):return _publish(store,user,generation,verified_units,fault)

def recover(store,user,*,edition_root=None):
    store,user=checked_roots(store,user)
    edition=edition_for_store(store,edition_root);require_edition_user(edition,user)
    with exclusive(edition):
        with exclusive_user_lock(user):return _recover(store,user)
