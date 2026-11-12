#!/usr/bin/env python3
"""Run real WebService handlers against a host-only ESP transport boundary."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--sanitize',action='store_true')
parser.add_argument('--cxx',default=os.environ.get('CXX','g++'))
parser.add_argument('--web-source',type=Path,default=ROOT/'src/web_service.cpp')
args=parser.parse_args()
build=ROOT/'build'/('http-v02-adapter-sanitize' if args.sanitize else 'http-v02-adapter')
build.mkdir(parents=True,exist_ok=True)
temporary=build/'tmp'
temporary.mkdir(exist_ok=True)
environment=os.environ.copy()
environment['TMPDIR']=str(temporary)
if args.sanitize:environment.setdefault('ASAN_OPTIONS','detect_leaks=0')
includes=[ROOT/'tests/http_adapter/stubs',ROOT/'src',ROOT/'lib/GatewayCore/src',ROOT/'lib/UpsCore/src',ROOT/'lib/SnmpCore/src',ROOT/'.pio/libdeps/esp32-poe-16mb/ArduinoJson/src']
sources=[args.web_source,ROOT/'tests/http_adapter/platform.cpp',ROOT/'tests/http_adapter/test_http.cpp']
sources+=[ROOT/'lib/GatewayCore/src'/name for name in ['auth_core.cpp','config_json.cpp','config_model.cpp','events.cpp','management_core.cpp']]
sources+=sorted((ROOT/'lib/UpsCore/src').glob('*.cpp'))
flags=['-std=c++17','-Wall','-Wextra','-Wpedantic','-Werror','-O1','-g','-DFIRMWARE_VERSION="0.2.0"','-DUPS_UART_TX=4','-DUPS_UART_RX=36']
if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-fno-omit-frame-pointer']
executable=build/'test_http'
subprocess.run([args.cxx,*flags,*['-I'+str(p) for p in includes],*[str(p) for p in sources],'-o',str(executable)],cwd=ROOT,check=True,env=environment)
subprocess.run([str(executable)],cwd=ROOT,check=True,env=environment)
