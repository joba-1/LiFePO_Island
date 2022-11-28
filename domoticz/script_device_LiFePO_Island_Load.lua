commandArray = {}

dev='LiFePO_Island_Load'
url='http://nat2:8080/'

switch = devicechanged[dev]
if (switch) then
    switch = string.lower(switch)
    print(dev .. ' ' .. switch)
    local http = require 'socket.http'
    response, status = http.request(url .. switch, '')
    if (status ~= 302) then
        print(dev .. ' request "' .. url .. switch .. '" failed with status ' .. status)
    end
end

return commandArray
