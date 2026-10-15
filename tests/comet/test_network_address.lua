package.path = arg[1] .. "/applet/?.lua;" .. package.path

local address = require("NetworkAddress")

local ipOutput = [[
1: lo: <LOOPBACK,UP> mtu 16436
    inet 127.0.0.1/8 scope host lo
3: eth1: <BROADCAST,MULTICAST,UP> mtu 1500
    inet 192.0.2.141/24 brd 192.168.1.255 scope global eth1
]]
assert(address.parse(ipOutput) == "192.0.2.141")

local ifconfigOutput = [[
lo Link encap:Local Loopback
  inet addr:127.0.0.1 Mask:255.0.0.0
eth1 Link encap:Ethernet
  inet addr:10.20.30.40 Bcast:10.20.30.255 Mask:255.255.255.0
]]
assert(address.parse(ifconfigOutput) == "10.20.30.40")
assert(address.parse("inet 127.0.0.1/8") == nil)
assert(address.parse("inet 999.1.1.1/24") == nil)

print("network address tests passed")
