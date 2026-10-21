local root=arg[1]
local lines,ticks={},100
package.preload["jive.utils.log"]=function() return {logger=function()
    return {setLevel=function(_,level) assert(level=="info") end,
        info=function(_,prefix,line) lines[#lines+1]=prefix..line end}
end} end
package.preload["jive.ui.Framework"]=function() return {getTicks=function() ticks=ticks+1;return ticks end} end
local trace=dofile(root.."/applet/HttpTrace.lua")
local Request={}
function Request:t_setResponseHeaders(status,line,headers) self.t_httpResponse.statusCode=status end
function Request:t_getResponseSink() return self.sink end
function Request:t_setResponseBody(data)
    local sink=self:t_getResponseSink()
    if sink then return sink(data) end
end
local Socket={fetch=function(self,request) self.t_httpRecvRequest=request end,close=function() end}
trace.apply({Socket=Socket,Request=Request})
local fetch=Socket.fetch
trace.apply({Socket=Socket,Request=Request});assert(Socket.fetch==fetch)
local socket=setmetatable({host="203.0.113.1",port=80},{__index=Socket})
local delivered={}
local function request(stream)
    return setmetatable({t_httpRequest={method="GET",uri={host="example.com",path="/logo",query="token=SECRET"},headers={Host="example.com"}},t_httpResponse={stream=stream},sink=function(data) delivered[#delivered+1]=data;return 1 end},{__index=Request})
end
local r=request(true)
socket:fetch(r)
assert(r.t_httpRequest.headers["X-SB-Trace-Id"]==r._standaloneTrace.id)
r:t_setResponseHeaders(200,"OK",{["Content-Type"]="image/png",Location="https://user:PASS@example.com/new?token=SECRET"})
r:t_setResponseBody("abc");r:t_setResponseBody("def");assert(not r._standaloneTrace.done)
r:t_setResponseBody(nil);socket:close("late close")
assert(r._standaloneTrace.bytes==6 and r._standaloneTrace.done and #delivered==2)
local completions=0
for _,line in ipairs(lines) do
    assert(not line:find("SECRET",1,true) and not line:find("PASS",1,true))
    assert(#line<1200)
    if line:find("event=complete id="..r._standaloneTrace.id.." ",1,true) then completions=completions+1;assert(line:find("bytes=6",1,true)) end
end
assert(completions==1)
local first=r._standaloneTrace.id
socket:fetch(r);assert(first~=r._standaloneTrace.id)
r:t_setResponseHeaders(302,"Found",{Location="/next?secret=x"});r:t_setResponseBody(nil)
local failed=request(false);socket:fetch(failed);socket:close("DNS or connect timeout\nunsafe")
assert(failed._standaloneTrace.done)
local errorRequest=request(false);socket:fetch(errorRequest);errorRequest:t_setResponseHeaders(404,"Not Found",{});errorRequest:t_setResponseBody("missing")
assert(lines[#lines]:find('error="HTTP 404"',1,true))
local sinkFailure=request(false);sinkFailure.sink=function() return nil,"write failed" end
socket:fetch(sinkFailure);sinkFailure:t_setResponseHeaders(200,"OK",{});sinkFailure:t_setResponseBody("abc")
assert(lines[#lines]:find("sink error: write failed",1,true))
trace.dns("example.com",nil,"nslookup failed")
local custom=request(false)
local chunks=0
custom.t_getResponseSinkMode=function() return "jive-by-chunk" end
custom.t_setResponseBody=function(_,data) if data then chunks=chunks+1 end end
socket:fetch(custom);custom:t_setResponseHeaders(200,"OK",{})
custom:t_setResponseBody("one");custom:t_setResponseBody("two")
assert(not custom._standaloneTrace.done)
custom:t_setResponseBody(nil)
assert(custom._standaloneTrace.done and custom._standaloneTrace.bytes==6 and chunks==2)
print("HTTP trace success, streaming, redirects, duplicate callbacks, errors, sink failures and redaction tests passed")
