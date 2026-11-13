#!/usr/bin/env python3
"""Reproducible target build and split-image release preparation."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
from flash import validate_images

ROOT=Path(__file__).resolve().parents[1]
def main():
    environment=os.environ.copy()
    environment['PLATFORMIO_SETTING_ENABLE_TELEMETRY']='no'
    subprocess.run([sys.executable,'-m','platformio','run','-e','esp32-poe-16mb'],cwd=ROOT,env=environment,check=True)
    source=ROOT/'.pio/build/esp32-poe-16mb'
    output=ROOT/'firmware';output.mkdir(exist_ok=True)
    names=('bootloader.bin','partitions.bin','firmware.bin')
    for name in names:shutil.copy2(source/name,output/name)
    hashes={name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in names}
    (output/'SHA256SUMS.txt').write_text(''.join(f'{hashes[name]}  {name}\n' for name in names),encoding='ascii')
    validate_images(output)
    info={'target':'ESP32-POE WROOM 16MB + MOD-RS232','firmware':'0.2.0','platformio':'6.1.18','platform':'espressif32@6.12.0','framework':'Arduino ESP32 2.0.17','arduinojson':'6.21.5','application_slots':2,'appcfg':{'offset':'0xF00000','size':'0x10000'},'images':{name:{'bytes':(output/name).stat().st_size,'sha256':hashes[name]} for name in names}}
    (output/'build-info.json').write_text(json.dumps(info,indent=2,ensure_ascii=False)+'\n')
    print('Prepared split images:',output)
    print('No combined flash image is generated; appcfg is excluded.')
if __name__=='__main__':
    try:main()
    except subprocess.CalledProcessError as error:sys.exit(error.returncode or 1)

