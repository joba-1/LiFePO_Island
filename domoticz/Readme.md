# Switch LiFePO_Island Load from Domoticz

With these lua scripts you can switch the load of the eSmart3 with domoticz.

## Install
* Copy the lua files to scripts/lua in your domoticz directory (mine is /opt/domoticz)
* Create a domoticz hardware of type "Dummy" (use any name, LiFePo_Island might make sense)
* Create a virtual sensor of type switch (german: Schalter) 
    * Use name LiFePO_Island_Load or adapt the script name and the dev variable inside the scripts
* Find the device index of the virtual switch (devices column "Idx" on the domoticz Settings/Devices page)
* Edit the idx variable in the script_time* file to match your device
* Edit the url in the script files to match your LiFePO_Island firmware web page

## Use
Now you have a switch in domoticz that controls the LiFePO_Island load. 
If you switch the load by other means, domoticz will pick up the new state within a minute
