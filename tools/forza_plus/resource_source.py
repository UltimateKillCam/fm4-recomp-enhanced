"""Automatic private original-resource discovery; never a distributed dump."""
from pathlib import Path
import hashlib,os,subprocess,uuid
from resource_codec import Invalid
from overlay_plan import safe_absolute,overlap
from prepared_database import digest
EXPECTED='9948f4053e2ebf91b3c998cb5dd49d5658f50cb9631a354ea86e2d4c5a512c98'
NATIVE='b34dfb88978819cccda6dafd755e7139970a58fbf6707dfbcba06846857f52bd'
IMAGE='1880c6edf326c0b7ff4b1060148edbe5e107b975002e45cc1d9e4b2082c52bcf'

def discover(edition,stock,helper=None):
    edition=safe_absolute(edition,Path.cwd());stock=safe_absolute(stock,edition)
    if digest(stock/'default.xex')!=NATIVE:raise Invalid('Native executable input differs')
    folder=edition/'.local/forza-plus/resource-export';output=folder/'native-original-resource.bin'
    safe_absolute(folder,edition)
    if overlap(folder,stock):raise Invalid('Private resource cache overlaps original game inputs')
    if output.is_file():
        safe_absolute(output,edition)
        if digest(output)!=EXPECTED:raise Invalid('Original resource cache is invalid; no unowned overwrite')
        return output
    # Existing source-build artifact, derived locally from the user's original
    # XEX. It is neither shipped nor a user-facing required technical step.
    image=edition/'.local/fm4-image.bin'
    if image.is_file() and digest(image)==IMAGE:
        raw=image.read_bytes()[0xea0000:0xea5771]
        if hashlib.sha256(raw).hexdigest()!=EXPECTED:raise Invalid('Local builder resource differs')
        folder.mkdir(parents=True,exist_ok=True);safe_absolute(folder,edition)
        temporary=folder/('resource-'+uuid.uuid4().hex+'.tmp')
        with temporary.open('xb') as stream:stream.write(raw);stream.flush();os.fsync(stream.fileno())
        os.replace(temporary,output);return output
    # Prefer the helper beside the edition's player executable: it shares that build's runtime DLLs.
    candidates=[Path(helper)] if helper else [edition/'.local/work/fm4/out/build/win-amd64-release/fm4-pfp-resource-export.exe',
        edition/'release/fm4-pfp-resource-export.exe',edition/'.local/bin/fm4-pfp-resource-export.exe']
    for candidate in candidates:
        if not candidate.is_file():continue
        # The launcher must select this helper from its verified release/build
        # manifest, just as it selects the matching game/runtime binaries.
        safe_absolute(candidate,edition)
        result=subprocess.run([str(candidate),str(edition),str(stock)],capture_output=True,text=True,timeout=120)
        if result.returncode or not output.is_file() or digest(output)!=EXPECTED:
            raise Invalid('Native CPU resource export failed: '+result.stdout+result.stderr)
        return output
    raise Invalid('native_resource_export_unavailable: verified CPU helper is not built/installed; enabled launch remains blocked')
