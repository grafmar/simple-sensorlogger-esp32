# simple-sensorlogger-esp8266
Logs multiple sensor data into local flash. Data can be accessed and displayed as graph in the WebUI running on the device.

## Overview
The device consists of an ESP8266 with several sensors connected. This device connects to the WLAN and can be accessed with a PC or smart phone or similar devices.

Device overview

The 
SW overview

## Acknowledgements
The idea of this project is based on "Examples/16. Data logging/A-Temperature_logger" of the https://github.com/tttapa/ESP8266/tree/master repository. The sensor sampling code and HTML code have been mostly changed and also the chart library has been replaced with chart.js. But that's where the idea and has come from.

Parts of this project were created with the assistance of AI tools
such as ChatGPT and Microsoft Copilot.

## Licence
The project is based on (Examples/16. Data logging/A-Temperature_logger)[https://github.com/tttapa/ESP8266/tree/master] which is licenced under GPL v3 and so is this project.

The Javascript libraries chart.js, hammerjs, chartjs-adapter-date-fns and chartjs-plugin-zoom are licenced under MIT licence. These are concatenated into chart_packed.js and than compressed to chart_packed.js.gz.
