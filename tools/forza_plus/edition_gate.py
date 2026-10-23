"""Windows CPU-only edition admission/game locks and explicit child handoff."""
from pathlib import Path
from contextlib import contextmanager
import ctypes,hashlib,os,uuid
from ctypes import wintypes
from overlay_plan import Invalid,safe_absolute

class Busy(Invalid):pass
kernel=ctypes.WinDLL('kernel32',use_last_error=True)
user=ctypes.WinDLL('user32',use_last_error=True)
for name,args,result in [
    ('CreateMutexW',[ctypes.c_void_p,wintypes.BOOL,wintypes.LPCWSTR],wintypes.HANDLE),
    ('CreateEventW',[ctypes.c_void_p,wintypes.BOOL,wintypes.BOOL,wintypes.LPCWSTR],wintypes.HANDLE),
    ('WaitForSingleObject',[wintypes.HANDLE,wintypes.DWORD],wintypes.DWORD),
    ('ReleaseMutex',[wintypes.HANDLE],wintypes.BOOL),('CloseHandle',[wintypes.HANDLE],wintypes.BOOL),
    ('SetEvent',[wintypes.HANDLE],wintypes.BOOL),
    ('GetFullPathNameW',[wintypes.LPCWSTR,wintypes.DWORD,wintypes.LPWSTR,ctypes.c_void_p],wintypes.DWORD)]:
    function=getattr(kernel,name);function.argtypes=args;function.restype=result
user.CharLowerBuffW.argtypes=[wintypes.LPWSTR,wintypes.DWORD];user.CharLowerBuffW.restype=wintypes.DWORD

def names(edition):
    edition=safe_absolute(edition,Path.cwd());buffer=ctypes.create_unicode_buffer(32768)
    length=kernel.GetFullPathNameW(str(edition),len(buffer),buffer,None)
    if not 0<length<len(buffer):raise Invalid('Edition canonical path invalid')
    value=buffer.value
    while len(value)>3 and value.endswith('\\'):value=value[:-1]
    lowered=ctypes.create_unicode_buffer(value);user.CharLowerBuffW(lowered,len(value))
    identity=hashlib.sha256(lowered.value.encode('utf-16le')).hexdigest()
    base='Local\\ProjectForza.Edition.'+identity
    return {'edition':edition,'admission':base+'.Admission','game':base+'.Game'}

class Mutex:
    def __init__(self,name):
        self.handle=kernel.CreateMutexW(None,False,name);self.owned=False;self.abandoned=False
        if not self.handle:raise ctypes.WinError(ctypes.get_last_error())
    def acquire(self,timeout_ms=0):
        status=kernel.WaitForSingleObject(self.handle,timeout_ms)
        if status in (0,0x80):self.owned=True;self.abandoned=status==0x80;return
        if status==258:raise Busy('This edition has an active game or launch/preparation transaction')
        raise ctypes.WinError(ctypes.get_last_error())
    def release(self):
        if self.owned:
            if not kernel.ReleaseMutex(self.handle):raise ctypes.WinError(ctypes.get_last_error())
            self.owned=False
    def close(self):
        if self.handle:self.release();kernel.CloseHandle(self.handle);self.handle=None

@contextmanager
def exclusive(edition):
    identity=names(edition);admission=Mutex(identity['admission']);game=None
    try:
        admission.acquire();game=Mutex(identity['game']);game.acquire()
        yield {'abandoned':admission.abandoned or game.abandoned,'identity':identity}
    finally:
        if game:game.close()
        admission.close()

def require_no_pending(edition):
    root=Path(edition)/'.local/user/.fm4-plus'
    for mode in ['normal','hard','sandbox','vanilla']:
        pending=safe_absolute(root/mode/'.fm4-plus-pending.json',edition)
        if pending.exists():raise Invalid('Edition has a pending content transaction; recover it before launch/preparation')

def launch_handoff(edition,start_child,timeout_ms=10000):
    """start_child receives a CHILD-ONLY env dict and returns its own Popen.

    Holds Admission until that child owns Game. It never terminates a process.
    A timeout signals the explicit cancellation event and reports failure.
    The native child checks cancellation before presentation/guest execution.
    """
    identity=names(edition);admission=Mutex(identity['admission']);game=None;ready=cancel=None
    try:
        admission.acquire();game=Mutex(identity['game']);game.acquire();require_no_pending(edition)
        nonce=uuid.uuid4().hex;ready_name='Local\\ProjectForza.LaunchReady.'+nonce;cancel_name='Local\\ProjectForza.LaunchCancel.'+nonce
        ready=kernel.CreateEventW(None,True,False,ready_name);cancel=kernel.CreateEventW(None,True,False,cancel_name)
        if not ready or not cancel:raise ctypes.WinError(ctypes.get_last_error())
        environment=dict(os.environ);environment.update(FM4_EDITION_ROOT=str(identity['edition']),FM4_LAUNCH_READY_EVENT=ready_name,FM4_LAUNCH_CANCEL_EVENT=cancel_name)
        process=start_child(environment)
        game.release() # Admission remains held across this handoff gap.
        if kernel.WaitForSingleObject(ready,timeout_ms)!=0:
            kernel.SetEvent(cancel)
            raise Busy('Started child did not acquire the edition game lease; cancellation signalled, no process killed')
        return process
    finally:
        if game:game.close()
        admission.close()
        if ready:kernel.CloseHandle(ready)
        if cancel:kernel.CloseHandle(cancel)
