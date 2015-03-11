
local function main()
	l = uv.listen6('::0', 8898, 10, function (cli)
		print('connected', cli._handle)

		local b = bytes.new(3)
		uv.read(cli, b, function (len)
			print(bytes.readUIntBE(b, 0, 1))
			print(bytes.readUIntBE(b, 1, 2))
		end)
	end)

	uv.setInterval(1000, function ()
	end)
end

local function testBytes()
	local b = bytes.new()
	bytes.appendUIntBE(b, 0x112244, 3)
	bytes.appendUIntBE(b, 0x332211, 3)
	bytes.appendUIntBE(b, 0x11, 3)
	for _, v in pairs(bytes.hexdump(b)) do
		print(v)
	end
end

local function testTimer()
	for i = 1, 2000 do
		uv.setTimeout(1000, function ()
		end)
	end
end

return function (loop)
	testTimer()
	uv.runLoop()
end

