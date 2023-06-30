udx = Proto("UDX",  "UDX Protocol")

udx.fields.magic_byte    = ProtoField.uint8("udx.magic_byte", "UDX Magic Byte", base.HEX)
udx.fields.version       = ProtoField.uint8("udx.version", "Version", base.DEC)
udx.fields.type          = ProtoField.uint8("udx.type", "Type", base.HEX)
udx.fields.type_data     = ProtoField.bool("udx.type.data", "Data", 8, nil , 0x1)
udx.fields.type_end      = ProtoField.bool("udx.type.end", "End", 8, nil, 0x2)
udx.fields.type_sack     = ProtoField.bool("udx.type.sack", "SACK", 8, nil, 0x4)
udx.fields.type_message  = ProtoField.bool("udx.type.message", "Message", 8, nil, 0x8)
udx.fields.type_destroy  = ProtoField.bool("udx.type.destroy", "Destroy", 8, nil, 0x10)
udx.fields.data_offset   = ProtoField.uint8("udx.data_offset", "Data Offset", base.DEC)
udx.fields.length        = ProtoField.uint32("udx.length", "Length", base.DEC)

udx.fields.id            = ProtoField.uint32("udx.id", "Id", base.DEC)
udx.fields.window        = ProtoField.uint32("udx.window", "Window", base.DEC)
udx.fields.seq           = ProtoField.uint32("udx.seq", "Seq", base.DEC)
udx.fields.ack           = ProtoField.uint32("udx.ack", "Ack", base.DEC)

udx.fields.sacks         = ProtoField.none("udx.sacks", "Sacks")
udx.fields.payload       = ProtoField.protocol("udx.payload", "UDX payload")

local TYPE_DATA = 1
local TYPE_END = 2
local TYPE_SACK = 4
local TYPE_MSG = 8
local TYPE_DESTROY = 16

local function get_type_names(type)
        local txt = "ACK,"

        if bit.band(type, TYPE_DATA) > 0 then txt = txt .. "DATA," end
        if bit.band(type, TYPE_END) > 0 then txt = txt .. "END," end
        if bit.band(type, TYPE_SACK) > 0 then txt = txt .. "SACK," end
        if bit.band(type, TYPE_MSG) > 0 then txt = txt .. "MSG," end
        if bit.band(type, TYPE_DESTROY) > 0 then txt = txt .. "DESTROY," end

        return txt:sub(1,-2)
end

function udx.dissector(tvb, pinfo, tree)
    local len = tvb:len()
    if len < 20 then return end

    pinfo.cols.protocol = udx.name
    local subtree = tree:add(udx, tvb(), "UDX Protocol")

    local type = tvb(2,1):uint()
    local type_names = get_type_names(type)

    subtree:add(udx.fields.magic_byte, tvb(0, 1))
    subtree:add(udx.fields.version, tvb(1,1))
    subtree:add(udx.fields.type, tvb(2,1)):append_text(" (" .. type_names .. ")")
    subtree:add(udx.fields.type_data, tvb(2,1))
    subtree:add(udx.fields.type_end, tvb(2,1))
    subtree:add(udx.fields.type_sack, tvb(2,1))
    subtree:add(udx.fields.type_message, tvb(2,1))
    subtree:add(udx.fields.type_destroy, tvb(2,1))
    subtree:add(udx.fields.data_offset, tvb(3,1))

    subtree:add_le(udx.fields.id, tvb(4,4))
    subtree:add_le(udx.fields.seq, tvb(12,4))
    subtree:add_le(udx.fields.ack, tvb(16,4))

    local data_offset = tvb(3,1):uint()
    local pos = 20

    if bit.band(type, TYPE_SACK) > 0 then
        local sacks = " "
        local header_end = data_offset > 0 and 20 + data_offset or len
        while pos + 8 <= header_end do
            local from = tvb(pos, 4):le_uint()
            local to = tvb(pos + 4, 4):le_uint()
            pos = pos + 8
            sacks = sacks .. from .. "-" .. to .. " "
        end
        sacks = sacks:sub(1, -2) -- trim off the trailing space
        subtree:add(udx.fields.sacks, tvb(20, header_end-20)):append_text(sacks)
    end

    if pos < len then
        subtree:add(udx.fields.length, len - pos):set_generated(true)
        subtree:add(udx.fields.payload, tvb(pos, len-pos))
    end

    local id = tvb(4,4):le_uint()
    local seq = tvb(12,4):le_uint()
    local ack = tvb(16,4):le_uint()
    local info = pinfo.src_port .. " → " .. pinfo.dst_port ..
        " Id=" .. id ..
        " Seq=" .. seq ..
        " Ack=" .. ack ..
        " " .. type_names
    pinfo.cols.info:set(info)
end

local function heuristic_checker(tvb, pinfo, tree)
    local len = tvb:len()
    if len < 20 then
        return false
    end

    local magic_byte = tvb(0, 1):uint()

    if magic_byte ~= 255 then
        return false
    end

    local version = tvb(1,1):uint()

    if version ~= 1 then
        return false
    end

    local flags = tvb(2,1):uint()

    if flags >= 32 then
        return false
    end

    local data_offset = tvb(3, 1):uint()

    if 20 + data_offset > len then
        return false
    end

    udx.dissector(tvb, pinfo, tree)
    return true
end

udx:register_heuristic('udp', heuristic_checker)
