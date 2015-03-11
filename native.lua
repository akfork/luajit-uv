
local ffi = require('ffi')
ffi.cdef(require('native_h'))

bytes = {}

bytes.new = function (len)
	if len == nil then len = 0 end
	local b = ffi.C.bytesNew(len)
	return ffi.gc(b, ffi.C.bytesFree)
end

bytes.fromStr = function (str)
	local len = string.len(str)
	local b = ffi.C.bytesNew(len)
	ffi.C.memcpy(ffi.C.bytesBuf(b), str, len)
	return b
end

bytes.id = function (b)
	return ffi.C.bytesId(b)
end

bytes.append = function (a, b)
	ffi.C.bytesAppend(a, b)
end

bytes.str = function (b, len)
	return ffi.string(ffi.C.bytesBuf(b), len)
end

bytes.len = function (b)
	return ffi.C.bytesLen(b)
end

bytes.readUIntBE = function (b, pos, len)
	return ffi.C.bytesReadIntBE(b, pos, len)
end

bytes.appendUIntBE = function (b, val, len)
	ffi.C.bytesAppendUIntBE(b, val, len)
end

bytes.hexdump = function (b)
	local r = {}
	local n = ffi.C.bytesLen(b)
	for i = 0, n, 16 do
		table.insert(r, ffi.string(ffi.C.bytesHexdump16(b, i)))
	end
	return r
end

uv = {}

-- 
-- valid = processing + ref
-- if incoming is not in processing then panic
--

uv._processingBytes = {}
uv._processing = {}
uv._valid = lua.newWeakTable()

uv._newHandle = function (_handle)
	local handle = {_handle = _handle}
	local id = ffi.C.uvGetId(_handle)
	uv._valid[id] = handle
	return handle
end

uv.setInterval = function (interval, cb)
	return uv.setTimer(interval, interval, cb)
end

uv.setTimeout = function (timeout, cb)
	return uv.setTimer(timeout, 0, cb)
end

uv.sleep = uv.setTimeout

uv.addToProcessing = function (handle) 
	uv._processing[ffi.C.uvGetId(handle._handle)] = handle
end

uv.removeFromProcessing = function (handle) 
	uv._processing[ffi.C.uvGetId(handle._handle)] = nil
end

uv.setTimer = function (timeout, repeat_, cb)
	local handle = uv._newHandle(ffi.C.uvSetTimer(timeout, repeat_))

	uv.addToProcessing(handle)
	handle.readCb = function ()
		if repeat_ == 0 then
			uv.removeFromProcessing(handle)
		end
		cb()
	end

	return handle
end

uv.listen6 = function (host, port, backlog, cb)
	local handle = uv._newHandle(ffi.C.uvTcpNew())
	ffi.C.uvListen6(handle._handle, host, port, backlog)

	uv.addToProcessing(handle)
	handle.connectCb = function (client, status)
		if status ~= 0 then return end
		cb(uv._newHandle(client))
	end
	return handle
end

uv.stopTimer = function (handle)
	ffi.C.uvStopTimer(handle._handle)
	uv.removeFromProcessing(handle)
end

uv.clearTimeout = uv.stopTimer
uv.clearInterval = uv.stopTimer

uv._immediateQueue = {}

uv._runImmediate = function () 
	while true do
		local k, cb = next(uv._immediateQueue)
		if k then
			uv._immediateQueue[k] = nil
			cb()
		else
			break
		end
	end
end

uv.setImmediate = function (cb)
	table.insert(uv._immediateQueue, cb)
end

uv.read = function (handle, b, cb)
	uv.addToProcessing(handle)
	uv._processingBytes[bytes.id(b)] = b

	handle.readCb = function (...)
		uv.removeFromProcessing(handle)
		uv._processingBytes[bytes.id(b)] = nil
		cb(...)
	end
	ffi.C.uvRead(handle._handle, b)
end

uv.write = function (handle, b, cb)
	uv.addToProcessing(handle)
	uv._processingBytes[bytes.id(b)] = b

	handle.writeCb = function (...)
		uv.removeFromProcessing(handle)
		uv._processingBytes[bytes.id(b)] = nil
		cb(...)
	end
	ffi.C.uvWrite(handle._handle, b)
end

uv._processEvent = function (e)
	local id = ffi.C.uvGetId(e.handle)
	local handle = uv._processing[id]

	if handle == nil then
		error('handle', e.handle, 'not in processing list')
	end

	if e.type == ffi.C.UV_E_READ then
		handle.readCb(e.args[0].i)
	elseif e.type == ffi.C.UV_E_WRITE then
		handle.writeCb()
	elseif e.type == ffi.C.UV_E_CONNECT then
		handle.connectCb(e.args[0].p, e.args[1].i)
	end

	uv._luaMemUsage = collectgarbage('count')
	
	--collectgarbage()

	ffi.C.uvWalkClearMark()
	for _, h in pairs(uv._valid) do
		ffi.C.uvMarkKeep(h._handle)
	end
	ffi.C.uvWalkGc()

	print('mem', uv._luaMemUsage)
	ffi.C.uvHandleDump()
end

uv.runLoop = function ()
	local e = ffi.new('uvEvent')

	while true do
		uv._runImmediate()

		local r = ffi.C.uvPollLoop(e)
		if r < 0 then 
			break 
		end
		if r > 0 then 
			uv._processEvent(e)
		end
	end
end

