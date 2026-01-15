#pragma once

// game independent gc constants
constexpr uint32_t ProtobufMask = 0x80000000;
constexpr uint32_t CCProtoMask = 0x90000000;

// Inventory Version
constexpr uint64_t InventoryVersion = 2000258; // the numbers mason, what do they mean? this one seems to make the client less schizo. (old comment: is this correct? taken from ClVe=SkinPlyr in pak_dat.vpk)

// Client Version
constexpr uint64_t ClientVersion = 102;

// Build timestamp - forces recompilation
#define BUILD_TIMESTAMP "v7.0-BATCHED"

// CMsgSOIDOwner type
enum SoIdType
{
    SoIdTypeSteamId = 1
};

