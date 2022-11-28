commandArray = {}

dev='LiFePO_Island_Load'
url='http://nat2:8080/json/LoadParam'
idx=2212

local http = require 'socket.http'
response, status = http.request(url)
if (status == 200) then
    load_sts=response:match('"LoadSts":([01]),')
    if (otherdevices[dev] == 'On' and load_sts == '0') then
        print(dev .. " -> off")
        commandArray['UpdateDevice'] = idx .. '|0|Off'
    elseif (otherdevices[dev] == 'Off' and load_sts == '1') then
        print(dev .. " -> on")
        commandArray['UpdateDevice'] = idx .. '|1|On'
    end
else
    print(dev .. ' request "' .. url .. '" failed with status ' .. status)
end

return commandArray