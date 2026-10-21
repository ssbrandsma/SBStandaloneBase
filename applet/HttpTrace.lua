-- Bounded metadata only: never retain response bodies or log credentials.
local M = {}
local log = require("jive.utils.log").logger("StandaloneHTTP")
local Framework = require("jive.ui.Framework")
local serial = 0
local epoch = tostring(os.time())
local function clean(value, limit)
    return tostring(value or "-"):gsub("[%c\"\\]", "_"):sub(1, limit or 512)
end
function M.redact(value)
    local text = tostring(value or "-")
    text = text:gsub("(https?://)[^/@]+@", "%1[redacted]@")
    return clean((text:gsub("[?#].*", "?[redacted]")))
end
local function emit(text) log:info("HTTPTRACE ", text) end
function M.begin(source, method, url, parent)
    serial = serial + 1
    local trace = {id="jive-"..epoch.."-"..serial, started=Framework:getTicks(), bytes=0}
    emit('event=request id='..trace.id..' parent='..clean(parent,80)..' source='..clean(source,80)..' method='..clean(method,12)..' url="'..M.redact(url)..'"')
    function trace:headers(status, values)
        if self.done then return end
        self.status = status
        local mapped={}
        for k,v in pairs(values or {}) do mapped[tostring(k):lower()]=v end
        emit('event=response id='..self.id..' status='..clean(status,12)..' type="'..clean(mapped["content-type"],128)..'" length='..clean(mapped["content-length"],24)..' location="'..M.redact(mapped.location)..'" duration_ms='..(Framework:getTicks()-self.started))
    end
    function trace:finish(err, bytes)
        if self.done then return end
        self.done=true
        emit('event=complete id='..self.id..' status='..clean(self.status,12)..' bytes='..tostring(bytes or self.bytes)..' duration_ms='..(Framework:getTicks()-self.started)..' error="'..M.redact(err)..'"')
    end
    function trace:result(outcome)
        emit('event=resource_result id='..self.id..' result="'..clean(outcome,256)..'"')
    end
    return trace
end
function M.dns(host, ip, err)
    emit('event=dns host="'..clean(host,253)..'" address='..clean(ip,64)..' error="'..M.redact(err)..'"')
end
function M.apply(deps)
    if log.setLevel then log:setLevel("info") end
    local radioLog=require("jive.utils.log").logger("StandaloneRadio")
    if radioLog.setLevel then radioLog:setLevel("info") end
    local Socket = deps and deps.Socket or require("jive.net.SocketHttp")
    if Socket._standaloneHttpTrace then return end
    local fetch, close = Socket.fetch, Socket.close
    Socket.fetch = function(self, request)
        -- Some Jive clients replace response methods on individual requests.
        -- Wrap those actual methods rather than only the RequestHttp class.
        if not request._standaloneTraceWrapped then
            local headers,body,getSink=request.t_setResponseHeaders,request.t_setResponseBody,request.t_getResponseSink
            request.t_setResponseHeaders=function(req,status,line,values)
                if req._standaloneTrace then req._standaloneTrace:headers(status,values) end
                return headers(req,status,line,values)
            end
            request.t_setResponseBody=function(req,data)
                local trace=req._standaloneTrace
                if trace then
                    if type(data)=="string" then trace.bytes=trace.bytes+#data end
                    local stream=req.t_getResponseSinkMode and req:t_getResponseSinkMode()=="jive-by-chunk" or req.t_httpResponse.stream
                    if data==nil or not stream then
                        trace:finish(tonumber(trace.status) and trace.status>=400 and ("HTTP "..trace.status) or nil)
                    end
                end
                return body(req,data)
            end
            request.t_getResponseSink=function(req)
                local sink=getSink(req)
                if not sink then return nil end
                return function(chunk,err,...)
                    if err and req._standaloneTrace then req._standaloneTrace:finish(err) end
                    local result,failure=sink(chunk,err,...)
                    if failure and req._standaloneTrace then req._standaloneTrace:result("sink error: "..M.redact(failure)) end
                    return result,failure
                end
            end
            request._standaloneTraceWrapped=true
        end
        local spec=request.t_httpRequest
        if spec then
            local uri=spec.uri or {}
            local host=(spec.headers or {}).Host or uri.host
            if not host or host=="" then host=self.host or self.address or "unknown" end
            local port=self.port
            if not tostring(host):find(":",1,true) and port and tonumber(port)~=80 then host=host..":"..port end
            local url=(uri.scheme or "http").."://"..host..(uri.path or "/")..(uri.query and ("?"..uri.query) or "")
            local previous=request._standaloneTrace
            request._standaloneTrace=previous and not previous.dispatched and previous or M.begin(tostring(self),spec.method,url,previous and previous.id)
            request._standaloneTrace.dispatched=true
            if spec.headers then spec.headers["X-SB-Trace-Id"]=request._standaloneTrace.id end
        end
        local ok,err=pcall(fetch,self,request)
        if not ok then
            if request._standaloneTrace then request._standaloneTrace:finish(err) end
            error(err)
        end
    end
    Socket.close = function(self,err)
        local function finish(request)
            if request and request._standaloneTrace then request._standaloneTrace:finish(err or "cancelled") end
        end
        finish(self.t_httpSendRequest);finish(self.t_httpRecvRequest)
        for _,request in ipairs(self.t_httpRecvRequests or {}) do finish(request) end
        return close(self,err)
    end
    Socket._standaloneHttpTrace=true
    emit("event=enabled source=jive")
end
return M
