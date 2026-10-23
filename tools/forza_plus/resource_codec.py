"""Independent CPU-only FM4 protected-file resource codec. No runtime hooks.

Derived from the actual supplied XEX/image bytes and the SDK's XEX resource
layout. No upstream adaptation implementation was copied.
"""
from dataclasses import dataclass
from pathlib import Path,PurePosixPath
import hashlib,math,re,struct
BLOCK=32768
RESOURCE=b'0D163575'
NATIVE_ENTRY=0x8275F9F0
OLD_ADDRESS=0x82EA0000
OLD_SIZE=0x5771
IMAGE_BASE=0x82000000
MAX_RESOURCE=1<<20
class Invalid(ValueError):pass
@dataclass(frozen=True)
class Entry:
    name:bytes
    size:int
    hashes:tuple[bytes,...]

def relative(name):
    try:text=name.decode('ascii').replace('\\','/')
    except UnicodeDecodeError as e:raise Invalid('Non-ASCII protected filename') from e
    if not text or text.startswith('/') or ':' in text or '\x00' in text:
        raise Invalid('Unsafe protected filename')
    parts=text.split('/')
    if any(x in ('','.','..') for x in parts):raise Invalid('Unsafe protected filename')
    return PurePosixPath(*parts)

def parse_table(data,expected_count=66):
    if len(data)<8:raise Invalid('Short signature header')
    block,count=struct.unpack_from('>II',data)
    if block!=BLOCK or count!=expected_count or count>1024:raise Invalid('Wrong signature version/count')
    start=8+8*count
    if start>len(data):raise Invalid('Truncated signature index')
    entries=[];spans=[];names=set()
    for index in range(count):
        offset,size=struct.unpack_from('>II',data,8+8*index)
        pos=start+offset
        if pos<start or pos>=len(data):raise Invalid('Invalid entry offset')
        end=data.find(b'\0',pos)
        if end<0:raise Invalid('Unterminated protected filename')
        name=data[pos:end];key=str(relative(name)).casefold()
        if key in names:raise Invalid('Duplicate protected filename')
        names.add(key)
        blocks=(size+BLOCK-1)//BLOCK
        last=end+1+32*blocks
        if last>len(data):raise Invalid('Truncated block hashes')
        if any(pos<right and left<last for left,right in spans):raise Invalid('Overlapping entries')
        spans.append((pos,last))
        hashes=tuple(data[end+1+32*i:end+1+32*(i+1)] for i in range(blocks))
        entries.append(Entry(name,size,hashes))
    return tuple(entries)

def encode_table(entries):
    entries=tuple(entries);payload=bytearray();index=bytearray()
    for entry in entries:
        relative(entry.name)
        if not 0<=entry.size<=0xffffffff:raise Invalid('Protected size exceeds guest uint32')
        if len(entry.hashes)!=(entry.size+BLOCK-1)//BLOCK or any(len(x)!=32 for x in entry.hashes):raise Invalid('Bad block hash count')
        index+=struct.pack('>II',len(payload),entry.size)
        payload+=entry.name+b'\0'+b''.join(entry.hashes)
    result=struct.pack('>II',BLOCK,len(entries))+index+payload
    if len(result)>MAX_RESOURCE:raise Invalid('Resource allocation limit exceeded')
    return result

def hash_file(name,path):
    path=Path(path);size=path.stat().st_size
    if size==0:raise Invalid('Empty replacement convention not proven by supplied FM4 table')
    if size>0xffffffff:raise Invalid('Protected size exceeds guest uint32')
    hashes=[]
    with path.open('rb') as source:
        while block:=source.read(BLOCK):hashes.append(hashlib.sha256(block).digest())
    if path.stat().st_size!=size or len(hashes)!=(size+BLOCK-1)//BLOCK:raise Invalid('Protected file changed during scan')
    return Entry(name,size,tuple(hashes))

def rebuild(entries,resolve):
    updated=tuple(hash_file(entry.name,resolve(relative(entry.name))) for entry in entries)
    return encode_table(updated),updated

def resource_directory(header):
    if len(header)<24 or header[:4]!=b'XEX2':raise Invalid('Not an XEX2 header')
    count=struct.unpack_from('>I',header,20)[0]
    if count>1024 or 24+8*count>len(header):raise Invalid('Bad XEX optional index')
    optional={}
    for i in range(count):
        key,value=struct.unpack_from('>II',header,24+8*i)
        if key in optional:raise Invalid('Duplicate XEX optional header')
        optional[key]=value
    if 0x2ff not in optional:raise Invalid('No XEX resource directory')
    base=optional[0x2ff]
    if base+4>len(header):raise Invalid('Resource directory outside XEX header')
    size=struct.unpack_from('>I',header,base)[0]
    if size<4 or (size-4)%16 or base+size>len(header):raise Invalid('Bad resource directory size')
    resources=[]
    for pos in range(base+4,base+size,16):
        name=bytes(header[pos:pos+8]);address,length=struct.unpack_from('>II',header,pos+8)
        resources.append((name,address,length,pos))
    return optional,tuple(resources)

def relocation_header(header,new_address,new_size):
    optional,resources=resource_directory(header)
    if optional.get(0x10100)!=NATIVE_ENTRY:raise Invalid('XEX differs from native recompilation input')
    selected=[x for x in resources if x[0]==RESOURCE]
    if len(selected)!=1 or selected[0][1:3]!=(OLD_ADDRESS,OLD_SIZE):raise Invalid('Wrong native protected resource')
    if new_address%16 or not 0<new_address<=0xffffffff or not 0<new_size<=MAX_RESOURCE or new_address+new_size>1<<32:
        raise Invalid('Invalid proposed guest allocation')
    if new_address<IMAGE_BASE+0xF50000 and IMAGE_BASE<new_address+new_size:raise Invalid('Allocation would overwrite native image')
    patched=bytearray(header);pos=selected[0][3]+8;struct.pack_into('>II',patched,pos,new_address,new_size)
    return bytes(patched),{'header_byte_offset':pos,'old_address':OLD_ADDRESS,'old_size':OLD_SIZE,'new_address':new_address,'new_size':new_size}

FOV_KEYS={'follow_low':b'FollowLowCam\\FOV','follow_high':b'FollowHighCam\\FOV','hood':b'HoodCam\\FOV','bumper_high':b'BumperHighCam\\FOV','driver':b'Driver\\FOV'}
def scalar_bytes(data,key,value):
    pattern=re.compile(rb'(?m)^('+re.escape(key)+rb'[ \t]+)([^\r\n \t]+)([ \t]*\r?)$')
    if len(pattern.findall(data))!=1:raise Invalid('Missing or duplicate exact scalar '+key.decode())
    return pattern.sub(lambda m:m[1]+str(value).encode('ascii')+m[3],data)

def camera_override(data,values):
    for name,value in values.items():
        if name not in FOV_KEYS:raise Invalid('Unknown camera field')
        if value is None:continue
        if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value) or not 0<value<180:raise Invalid('FOV must be finite degrees between0 and180')
        data=scalar_bytes(data,FOV_KEYS[name],format(value,'.9g'))
    return data

def custom_physics(data,target_fps):
    if isinstance(target_fps,bool) or not isinstance(target_fps,(int,float)) or not math.isfinite(target_fps) or not 1<=target_fps<=1000:raise Invalid('Invalid custom physics target FPS')
    # Documented positive round-to-nearest rule, independent of display/FG FPS.
    hz=math.floor(float(target_fps)*5.79+0.5)
    return scalar_bytes(data,b'CarPhysicsUpdateHz',hz),hz
