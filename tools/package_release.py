#!/usr/bin/env python3
"""Package complete source, human docs and verified split images, excluding local state."""
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import os
import shutil
import sys
import tempfile
import zipfile
from flash import validate_images
from release_gate import fingerprint, inventory, run as run_release_gate

ROOT=Path(__file__).resolve().parents[1]
ARCHIVE='esp32-ups-gateway-v0.2.0.zip'


def main():
    destination=ROOT/'dist'
    archive=destination/ARCHIVE
    if archive.exists():
        stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
        history=archive.with_name(archive.name+'.superseded-'+stamp)
        if history.exists():raise FileExistsError(history)
        archive.rename(history)
    with tempfile.TemporaryDirectory(prefix='esp32-release-snapshot-') as temporary:
        snapshot=Path(temporary)
        files=inventory(ROOT)
        for name in files:
            target=snapshot/name
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/name,target)
        before=fingerprint(snapshot)
        validate_images(snapshot/'firmware')
        if not run_release_gate(snapshot):return 1
        if fingerprint(snapshot)!=before:raise ValueError('release snapshot changed during checks')
        destination.mkdir(exist_ok=True)
        descriptor,temp_name=tempfile.mkstemp(prefix='.'+ARCHIVE+'.',suffix='.tmp',dir=destination)
        os.close(descriptor)
        try:
            with zipfile.ZipFile(temp_name,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as output:
                for name in files:
                    output.write(snapshot/name,Path('esp32-ups-gateway')/name)
            with zipfile.ZipFile(temp_name) as output:
                expected=[(Path('esp32-ups-gateway')/name).as_posix() for name in files]
                if output.testzip() is not None or sorted(output.namelist())!=sorted(expected):
                    raise ValueError('release archive verification failed')
            if fingerprint(snapshot)!=before:raise ValueError('release snapshot changed while archiving')
            checksum=hashlib.sha256(Path(temp_name).read_bytes()).hexdigest()
            size=Path(temp_name).stat().st_size
            os.replace(temp_name,archive)
        finally:
            if os.path.exists(temp_name):os.unlink(temp_name)
    print(archive);print('SHA256',checksum);print('Bytes',size)
    return 0
if __name__=='__main__':
    if len(sys.argv)!=1:sys.exit('This command accepts no options')
    raise SystemExit(main())
