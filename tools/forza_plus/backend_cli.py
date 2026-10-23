"""Private JSON request/response bridge. No operation runs implicitly.

The launcher owns the separate global GPU lock and must retain it until the
ACTUAL game PID exits; the edition lease is a different, CPU/data safety lock.
"""
from pathlib import Path
import argparse,json,os,struct,subprocess,sys
from private_prepare import prepare_private,verify_generation,normalize
from overlay_plan import safe_absolute,Invalid
from publication import publish,manifest,matches,read_json,atomic_json
from edition_gate import exclusive,require_no_pending,launch_handoff

PROTOCOL='fm4-plus-proposal/1'

def pe_exports(path):
    data=Path(path).read_bytes()
    def u16(at):return struct.unpack_from('<H',data,at)[0]
    def u32(at):return struct.unpack_from('<I',data,at)[0]
    if data[:2]!=b'MZ':return set()
    pe=u32(0x3c)
    if data[pe:pe+4]!=b'PE\0\0':return set()
    count=u16(pe+6);optional=pe+24;size=u16(pe+20);magic=u16(optional)
    directory=optional+(112 if magic==0x20b else 96)
    if magic not in (0x10b,0x20b) or count>96:return set()
    sections=[struct.unpack_from('<IIII',data,optional+size+i*40+8) for i in range(count)]
    def offset(rva):
        for virtual_size,virtual,raw_size,raw in sections:
            if virtual<=rva<virtual+max(virtual_size,raw_size):
                at=raw+rva-virtual
                if at>=len(data):raise Invalid('Invalid PE export mapping')
                return at
        raise Invalid('Unmapped PE export RVA')
    export_rva=u32(directory)
    if not export_rva:return set()
    export=offset(export_rva);names=u32(export+24);pointers=offset(u32(export+32))
    if names>65536:raise Invalid('PE export count invalid')
    result=set()
    for i in range(names):
        start=offset(u32(pointers+4*i));end=data.find(b'\0',start,start+512)
        if end<0:raise Invalid('Unterminated PE export name')
        result.add(data[start:end].decode('ascii'))
    return result

def canonical_options(edition,settings):
    result=normalize(settings)
    if not result['enabled']:return result
    for key in ['source_folder','update_folder','prepared_game_folder']:
        if result[key]:result[key]=str(safe_absolute(result[key],edition))
    return result

def inspect(request):
    edition=safe_absolute(request['edition_root'],Path.cwd());settings=request['settings'];options=canonical_options(edition,settings)
    if not options['enabled']:
        return {'status':'disabled','enabled_requested':False,'launch_allowed':True,'game_data_root':str(edition/'.local/game-disc1'),'user_data_root':str(edition/'.local/user')}
    executable=Path(request['game_executable']) if request.get('game_executable') else edition/'release/fm4.exe'
    capable=executable.is_file() and 'Fm4ForzaPlusNativeProtocolV1' in pe_exports(executable)
    result={'enabled_requested':True,'launch_allowed':False,'native_hook_available':capable,'native_compatibility_validated':False}
    store=Path(options['prepared_game_folder']);active=read_json(store/'active.json')
    if active is None:return {**result,'status':'prepared_missing'}
    identity=active.get('generation')
    if not isinstance(identity,str) or len(identity)!=32 or any(x not in '0123456789abcdef' for x in identity):raise Invalid('Active generation identity invalid')
    generation=store/'versions'/identity;receipt=verify_generation(generation,quick=bool(request.get('quick_verify',True)))
    if receipt['options']!=options:return {**result,'status':'options_changed','receipt_id':identity}
    user=edition/'.local/user/.fm4-plus'/options['mode'];require_no_pending(edition)
    if active.get('protocol')!='fm4-plus-publication/1' or active.get('user')!=str(user):raise Invalid('Active route differs from mode receipt')
    owner=read_json(user/'.fm4-plus-content.json')
    if owner is None or owner.get('generation')!=identity or owner.get('user')!=str(user) or owner.get('store')!=str(store):raise Invalid('Published content owner differs')
    if set(owner.get('units',{}))!=set(receipt['content_units']):raise Invalid('Published content units differ')
    for name,wanted in owner['units'].items():
        quick=bool(request.get('quick_verify',True))
        if not matches(user/Path(name),wanted,quick) or not matches(generation/'content'/Path(name),wanted,quick):raise Invalid('Published content changed')
    return {**result,'status':'ready' if capable else 'native_hook_unavailable','launch_allowed':capable,'receipt_id':identity,
        'generation':str(generation),'game_data_root':str(generation/'game'),'user_data_root':str(user),
        'eligible_content_packages':[x for x in receipt['packages'] if x['eligible']],
        'omitted_content_packages':[x for x in receipt['packages'] if not x['eligible']]}

def execute(request):
    if request.get('protocol')!=PROTOCOL:raise Invalid('Wrong native bridge protocol')
    operation=request.get('operation');edition=safe_absolute(request['edition_root'],Path.cwd())
    if operation=='select_stock':return {'status':'disabled','launch_allowed':True,'game_data_root':str(edition/'.local/game-disc1'),'user_data_root':str(edition/'.local/user')}
    if operation in ('inspect','verify'):
        if request['settings'].get('enabled') is False:return inspect(request)
        with exclusive(edition):return inspect(request)
    if operation=='prepare':
        if request['settings'].get('enabled') is False:return inspect(request)
        with exclusive(edition):
            result=prepare_private(edition,request['settings'])
            generation=Path(result['generation']);receipt=verify_generation(generation)
            publish(receipt['store'],result['user_data_root'],result['receipt_id'],receipt['content_units'],edition_root=edition)
            return inspect(request)
    if operation=='launch':
        if request['settings'].get('enabled') is not True:raise Invalid('Stock launch stays on the existing launcher path')
        state={}
        def start(environment):
            state.update(inspect(request))
            if not state.get('launch_allowed'):raise Invalid('Forza Plus launch blocked: '+state['status'])
            for key in request.get('game_environment_remove',[]) or []:environment.pop(str(key),None)
            environment.update({str(k):str(v) for k,v in request.get('game_environment',{}).items()})
            environment.update(FM4_EDITION_ROOT=str(edition),FM4_FORZA_PLUS_EDITION_ROOT=str(edition),FM4_FORZA_PLUS_GENERATION=state['generation'])
            arguments=request.get('game_arguments',[])
            if not isinstance(arguments,list) or any(not isinstance(x,str) or '\0' in x for x in arguments):raise Invalid('Game arguments must be a string array')
            return subprocess.Popen([str(Path(request['game_executable'])),*arguments],env=environment,cwd=Path(request['game_executable']).parent)
        process=launch_handoff(edition,start)
        result={**state,'status':'started','pid':process.pid}
        if request.get('wait_for_exit',True):result.update(exit_code=process.wait(),status='closed')
        return result
    raise Invalid('Unknown native bridge operation')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--request',required=True);parser.add_argument('--response');args=parser.parse_args()
    try:
        result={'protocol':PROTOCOL,**execute(json.loads(Path(args.request).read_text(encoding='utf-8-sig')))};code=0
    except Exception as error:result={'protocol':PROTOCOL,'status':'failed','launch_allowed':False,'diagnostics':[{'code':type(error).__name__,'message':str(error)}]};code=2
    if args.response:atomic_json(Path(args.response),result)
    print(json.dumps(result,separators=(',',':')));sys.exit(code)
