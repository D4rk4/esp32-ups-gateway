#!/usr/bin/env python3
"""Package complete source, human docs and verified split images, excluding local state."""
from pathlib import Path
import hashlib
import os
import sys
import tempfile
import zipfile
from flash import validate_images
from release_gate import copy_snapshot, file_digests, fingerprint, inventory, run as run_release_gate

ROOT=Path(__file__).resolve().parents[1]
ARCHIVE='esp32-ups-gateway-v0.2.0.zip'


def main():
    destination=ROOT/'dist'
    archive=destination/ARCHIVE
    if destination.is_symlink() or os.path.lexists(archive):
        existing=destination if destination.is_symlink() else archive
        print(f'{existing}: already exists or is a symlink and is NOT the result of this run')
        return 1
    with tempfile.TemporaryDirectory(prefix='esp32-release-snapshot-') as temporary:
        snapshot=Path(temporary)
        files=inventory(ROOT)
        for name in files:
            path=Path(name)
            if path.parts[0]=='firmware' and path.suffix.lower()=='.bin' and name not in (
                    'firmware/bootloader.bin','firmware/partitions.bin','firmware/firmware.bin'):
                raise ValueError(f'unvalidated firmware binary: {name}')
        copy_snapshot(ROOT,snapshot,files)
        digests=file_digests(snapshot)
        before=fingerprint(snapshot,digests)
        validate_images(snapshot/'firmware')
        if not run_release_gate(snapshot):return 1
        if fingerprint(snapshot)!=before:raise ValueError('release snapshot changed during checks')
        destination.mkdir(exist_ok=True)
        if destination.is_symlink():raise ValueError(f'distribution directory is a symlink: {destination}')
        descriptor,temp_name=tempfile.mkstemp(prefix='.'+ARCHIVE+'.',suffix='.tmp',dir=destination)
        try:
            os.close(descriptor)
            with zipfile.ZipFile(temp_name,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as output:
                for name in files:
                    output.write(snapshot/name,Path('esp32-ups-gateway')/name)
            recorded={(Path('esp32-ups-gateway')/name).as_posix():digest for name,digest in digests.items()}
            with zipfile.ZipFile(temp_name) as output:
                entries=output.namelist()
                if output.testzip() is not None or sorted(entries)!=sorted(recorded):
                    raise ValueError('release archive verification failed')
                for name in entries:
                    if hashlib.sha256(output.read(name)).hexdigest()!=recorded[name]:
                        raise ValueError('release archive content mismatch')
            if fingerprint(snapshot)!=before:raise ValueError('release snapshot changed while archiving')
            checksum=hashlib.sha256(Path(temp_name).read_bytes()).hexdigest()
            size=Path(temp_name).stat().st_size
            if destination.is_symlink():raise ValueError(f'distribution directory is a symlink: {destination}')
            try:
                os.link(temp_name,archive)
            except FileExistsError:
                print(f'{archive}: already exists and is NOT the result of this run')
                return 1
        finally:
            if os.path.exists(temp_name):os.unlink(temp_name)
    print(archive);print('SHA256',checksum);print('Bytes',size)
    return 0
if __name__=='__main__':
    if len(sys.argv)!=1:sys.exit('This command accepts no options')
    raise SystemExit(main())
