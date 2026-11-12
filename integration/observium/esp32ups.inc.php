<?php
// Install as /opt/observium/includes/definitions/os/esp32ups.inc.php.
// Current official reference: https://docs.observium.org/developing/definitions/
// Local profile; validate discovery on the exact installed CE release.
$os = 'esp32-ups-gateway';
$config['os'][$os]['text'] = 'ESP32 UPS Gateway';
$config['os'][$os]['type'] = 'power';
$config['os'][$os]['vendor'] = 'ESP32 UPS Gateway';
$config['os'][$os]['sysDescr'][] = '/^ESP32 UPS Gateway\//';
$config['os'][$os]['mibs'][] = 'PowerNet-MIB';
// sysObjectID is deliberately zeroDotZero (0.0); never use it as a match.
