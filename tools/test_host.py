#!/usr/bin/env python3
"""Build and run real platform-independent cores. No UPS or ESP32 is simulated as tested."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parent.parent
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--sanitize',action='store_true')
    parser.add_argument('--cxx',default=os.environ.get('CXX','g++'))
    args=parser.parse_args()
    arduino=ROOT/'.pio/libdeps/esp32-poe-16mb/ArduinoJson/src'
    if not (arduino/'ArduinoJson.h').exists():
        parser.error('ArduinoJson not installed: run pio pkg install first')
    build=ROOT/'build'/('host-sanitize' if args.sanitize else 'host')
    build.mkdir(parents=True,exist_ok=True)
    temporary=build/'tmp';temporary.mkdir(exist_ok=True)
    environment=os.environ.copy();environment['TMPDIR']=str(temporary)
    include=[ROOT/'lib/UpsCore/src',ROOT/'lib/SnmpCore/src',ROOT/'lib/GatewayCore/src',arduino]
    sources=sorted(str(p) for path in include[:3] for p in path.glob('*.cpp'))
    flags=['-std=c++17','-Wall','-Wextra','-Wpedantic','-Werror','-O1','-g']
    if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    tests=sorted((ROOT/'tests').glob('test_*.cpp'))
    # Rebuild common production code once per invocation with the same flags.
    # This is not a stale object cache: every source is compiled on every run.
    objects=[]
    for source in sources:
        relative=Path(source).relative_to(ROOT)
        obj=build/('_'.join(relative.parts)+'.o')
        print('COMPILE',relative.as_posix(),flush=True)
        subprocess.run([args.cxx,*flags,*['-I'+str(p) for p in include],'-c',source,'-o',str(obj)],check=True,cwd=ROOT,env=environment)
        objects.append(str(obj))
    for test in tests:
        executable=build/test.stem
        command=[args.cxx,*flags,*['-I'+str(p) for p in include],str(test),*objects,'-o',str(executable)]
        print('BUILD',test.name,flush=True);subprocess.run(command,check=True,cwd=ROOT,env=environment)
        print('RUN',test.name,flush=True);subprocess.run([str(executable)],check=True,cwd=ROOT,env=environment)
    print(f'PASS: {len(tests)} native suites',flush=True)
    return 0
if __name__=='__main__':
    try:sys.exit(main())
    except subprocess.CalledProcessError as error:sys.exit(error.returncode or 1)
