
printTraceback = function (err)
	print(err, debug.traceback())
end

lua = {}
lua.pcall = function (f)
	local r, err = pcall(f)
	if type(err) == 'string' then
		printTraceback(err)
	end
end

lua.newWeakTable = function ()
	local t = {}
	setmetatable(t, { __mode = 'v' })
	return t
end

err = {}
err.IO = {}

co = {}

co.throw = function (e)
	coroutine.yield({type='err', err=e})
end

co.await = function (fn, ...)
	return coroutine.yield({fn=fn, args={...}})
end

co.async = function (fn)
	local th = coroutine.create(fn)
	local res = {}

	local function loop(cb)
		local ok, v = coroutine.resume(th, unpack(res))
		local done = coroutine.status(th) == 'dead'

		if done then 
			if not ok then
				print(v, debug.traceback(th))
				error('')
			else
				if cb then cb(v) end
			end
		else
			if v.type == 'err' then
				if cb then cb(nil, v.err) end
				return
			end
			local called
			table.insert(v.args, function (...)
				if called then error('cb you pass to co.await called twice') end
				called = true
				res = {...}
				loop(cb)
			end)
			v.fn(unpack(v.args))
		end
	end

	return loop
end

require('native')

return function (loop) 
	lua.pcall(function () require('server')(loop) end)
end

