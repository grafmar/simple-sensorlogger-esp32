# simple-sensorlogger-esp8266
Logs multiple sensor data into local flash. Data can be accessed and displayed as graph in the WebUI running on the device.

## Overview
The device consists of an ESP8266 with several sensors connected. This device connects to the WLAN and can be accessed with a PC or smart phone or similar devices.

<img src="doc/device_overview.png" alt="Device Overview" width="75%"/>


The webserver running on the ESP8266 serves the files directly from the filesystem (LittleFS). All the files are ready to use on the filesystem and no active code has to be executed to show the webpage with the sensor data.

On the other hand the sensor sampler samples all the sensors every configured period and stores the data into data.csv on the LittleFS. This file can than be accessed directly from the webpage.

<img src="doc/sw_overview.png" alt="SW Overview" width="75%"/>


The webpage showing a chart of the sensordata with enabled pan and zoom. The data can be hidden/shown using the checkboxes on the top.

<img src="doc/webpage.png" alt="Webpage" width="75%"/>


## Acknowledgements
The idea of this project is based on "Examples/16. Data logging/A-Temperature_logger" of the https://github.com/tttapa/ESP8266/tree/master repository. The sensor sampling code and HTML code have been mostly changed and also the chart library has been replaced with chart.js. But that's where the idea and has come from.

Parts of this project were created with the assistance of AI tools
such as ChatGPT and Microsoft Copilot.

## Licence
The project is based on (Examples/16. Data logging/A-Temperature_logger)[https://github.com/tttapa/ESP8266/tree/master] which is licenced under GPL v3 and so is this project.

The Javascript libraries chart.js, hammerjs, chartjs-adapter-date-fns and chartjs-plugin-zoom are licenced under MIT licence. These are concatenated into chart_packed.js and than compressed to chart_packed.js.gz.
