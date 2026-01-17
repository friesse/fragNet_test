#include "networking_inventory.hpp"
// Split files
// networking_inventory_actions.cpp
// networking_inventory_transactions.cpp
#include "econ_gcmessages.pb.h"
#include "gc_const_csgo.hpp"
#include "gcsystemmsgs.pb.h"
#include "keyvalue_english.hpp"
#include "logger.hpp"
#include "networking_users.hpp"
#include "prepared_stmt.hpp"
#include "safe_parse.hpp"
#include "tunables_manager.hpp"
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

// Global ItemSchema - now RAII managed with unique_ptr (#3 fix)
std::unique_ptr<ItemSchema> g_itemSchema;

bool GCNetwork_Inventory::Init() {
  if (g_itemSchema != nullptr) {
    return true;
  }

  // note: localization initializes on first use through
  // LocalizationSystem::GetInstance()

  // init ItemSchema - using make_unique for exception safety
  g_itemSchema = std::make_unique<ItemSchema>();
  if (g_itemSchema) {
    logger::info(
        "GCNetwork_Inventory::Init: ItemSchema initialized successfully");

    // Verify that localization system is working
    std::string_view testString = LocalizeToken("SFUI_WPNHUD_SSG08", "Scout");
    logger::info(
        "GCNetwork_Inventory::Init: Localization test - SSG08 resolves to '%s'",
        std::string{testString}.c_str());
    return true;
  } else {
    logger::error(
        "GCNetwork_Inventory::Init: Failed to create ItemSchema instance");
    return false;
  }
}

void GCNetwork_Inventory::Cleanup() {
  // unique_ptr handles deletion automatically, but we can still reset
  // explicitly
  g_itemSchema.reset();
}

/**
 * Parses an item ID string to extract definition index and paint index
 *
 * @param item_id String representation of the item ID (e.g. "skin_123_456")
 * @param def_index Output parameter for the definition index
 * @param paint_index Output parameter for the paint index
 * @return True if parsing was successful, false otherwise
 */
bool GCNetwork_Inventory::ParseItemId(const std::string &item_id,
                                      uint32_t &def_index,
                                      uint32_t &paint_index) {
  try {
    size_t dash_pos = item_id.find('-');
    if (dash_pos != std::string::npos) {
      std::string type = item_id.substr(0, dash_pos);
      std::string number_part = item_id.substr(dash_pos + 1);
      uint32_t item_number = std::stoi(number_part);

      if (type == "music_kit") {
        def_index = 1314;
        paint_index = item_number; // just reusing paint_index cause uhhh yes
        return true;
      } else if (type == "sticker") {
        def_index = 1209;
        paint_index = item_number;
        return true;
      } else if (type == "crate") {
        def_index = item_number;
        paint_index = 0;
        return true;
      } else if (type == "key") {
        def_index = item_number;
        paint_index = 0;
        return true;
      } else if (type == "collectible") {
        def_index = item_number;
        paint_index = 0;
        return true;
      }
    }

    // weapon skins:
    size_t first_underscore = item_id.find('_');
    size_t second_underscore = item_id.find('_', first_underscore + 1);
    if (first_underscore == std::string::npos ||
        second_underscore == std::string::npos) {
      logger::error("ParseItemId: Failed to find required underscores");
      return false;
    }

    std::string def_index_str = item_id.substr(5, first_underscore - 5);
    std::string paint_index_str = item_id.substr(
        first_underscore + 1, second_underscore - first_underscore - 1);
    def_index = std::stoi(def_index_str);
    paint_index = std::stoi(paint_index_str);

    // logger::info("ParseItemId: parsed item def_index: %u, paint_index: %u",
    // def_index, paint_index);
    return true;
  } catch (const std::exception &e) {
    logger::error("ParseItemId: Exception caught: %s", e.what());
    return false;
  } catch (...) {
    logger::error("ParseItemId: Unknown exception caught");
    return false;
  }
}

/**
 * Adds sticker attributes to an item based on database values
 *
 * @param item Pointer to the CSOEconItem to modify
 * @param row MySQL row containing sticker data
 * @param sticker_index Index of the sticker position (0-4)
 */
void GCNetwork_Inventory::AddStickerAttributes(CSOEconItem *item,
                                               MYSQL_ROW &row,
                                               int sticker_index) {
  int sticker_col = 8 + (sticker_index * 2);
  int sticker_wear_col = sticker_col + 1;

  auto stickerVal = SafeParse::toInt(row[sticker_col]);
  if (row[sticker_col] && stickerVal.value_or(0) > 0) {
    uint32_t sticker_id_attr = 113 + (sticker_index * 4);
    uint32_t sticker_wear_attr = sticker_id_attr + 1;

    // sticker_id
    AddUint32Attribute(item, sticker_id_attr, stickerVal.value());

    // sticker_wear
    if (row[sticker_wear_col] && strlen(row[sticker_wear_col]) > 0) {
      AddFloatAttribute(item, sticker_wear_attr,
                        std::stof(row[sticker_wear_col]));
    } else {
      AddFloatAttribute(item, sticker_wear_attr, 0.0f);
    }
  }
}

/**
 * Adds equipped state to an item if it is equipped
 *
 * @param item Pointer to the CSOEconItem to modify
 * @param equipped Boolean indicating if the item is equipped
 * @param class_id Class ID for which the item is equipped (CT/T)
 * @param def_index Definition index of the item
 */
void GCNetwork_Inventory::AddEquippedState(CSOEconItem *item, bool equipped,
                                           uint32_t class_id,
                                           uint32_t def_index) {
  if (equipped) {
    auto equipped_state = item->add_equipped_state();
    equipped_state->set_new_class(class_id);
    equipped_state->set_new_slot(GetItemSlot(def_index));
  }
}

/**
 * Determines the appropriate slot ID for a given defindex
 *
 * @param defIndex The definition index of the item
 * @return The slot ID where this item should be equipped
 */
uint32_t GCNetwork_Inventory::GetItemSlot(uint32_t defIndex) {
  if (defIndex >= 500 && defIndex <= 552)
    return 0; // Custom Knives
  if (defIndex == 42 || defIndex == 59)
    return 0; // Default knives

  if (defIndex == 49)
    return 1; // C4

  // Pistols
  if (defIndex == 4 || defIndex == 32 || defIndex == 61)
    return 2; // Glock / P2000 / USP-S
  if (defIndex == 2)
    return 3; // Dual Berettas
  if (defIndex == 36)
    return 4; // P250
  if (defIndex == 3 || defIndex == 30 || defIndex == 63)
    return 5; // Five-SeveN / Tec-9 / CZ75
  if (defIndex == 1 || defIndex == 64)
    return 6; // Deagle / R8

  // SMGs
  if (defIndex == 34 || defIndex == 17)
    return 8; // MP9 / MAC-10
  if (defIndex == 33 || defIndex == 23)
    return 9; // MP7 / MP5-SD
  if (defIndex == 24)
    return 10; // UMP-45
  if (defIndex == 19)
    return 11; // P90
  if (defIndex == 26)
    return 12; // PP-Bizon

  // Rifles
  if (defIndex == 10 || defIndex == 13)
    return 14; // FAMAS / Galil AR
  if (defIndex == 7 || defIndex == 16 || defIndex == 60)
    return 15; // AK-47 / M4A4 / M4A1-S
  if (defIndex == 40)
    return 16; // SSG 08 (Scout)
  if (defIndex == 39 || defIndex == 8)
    return 17; // SG 553 / AUG
  if (defIndex == 9)
    return 18; // AWP
  if (defIndex == 11 || defIndex == 38)
    return 19; // G3SG1 / SCAR-20

  // Heavy
  if (defIndex == 35)
    return 20; // Nova
  if (defIndex == 25)
    return 21; // XM1014
  if (defIndex == 29 || defIndex == 27)
    return 22; // Sawed-Off / MAG-7
  if (defIndex == 14)
    return 23; // M249
  if (defIndex == 28)
    return 24; // Negev

  // no gloves cuz its classiccounter

  if (defIndex == 1314)
    return 54; // Music kits

  return 55; // probably a collectible
}

/**
 * Returns a vector of defindexes that correspond to the given item slot
 * This is the reverse of GetItemSlot
 *
 * @param slotId The slot ID to look up
 * @return Vector of definition indexes that would be equipped in this slot
 */
std::vector<uint32_t>
GCNetwork_Inventory::GetDefindexFromItemSlot(uint32_t slotId) {
  std::vector<uint32_t> result;

  switch (slotId) {
  case 0: // Knives
    // Default knives
    result.push_back(42); // Default CT Knife
    result.push_back(59); // Default T Knife

    // Custom knives (500-552)
    for (uint32_t i = 500; i <= 552; i++) {
      result.push_back(i);
    }
    break;

  case 1: // C4
    result.push_back(49);
    break;

  case 2:                 // Glock / P2000 / USP-S
    result.push_back(4);  // Glock-18
    result.push_back(32); // P2000
    result.push_back(61); // USP-S
    break;

  case 3: // Dual Berettas
    result.push_back(2);
    break;

  case 4: // P250
    result.push_back(36);
    break;

  case 5:                 // Five-SeveN / Tec-9 / CZ75
    result.push_back(3);  // Five-SeveN
    result.push_back(30); // Tec-9
    result.push_back(63); // CZ75-Auto
    break;

  case 6:                 // Deagle / R8
    result.push_back(1);  // Desert Eagle
    result.push_back(64); // R8 Revolver
    break;

  case 8:                 // MP9 / MAC-10
    result.push_back(34); // MP9
    result.push_back(17); // MAC-10
    break;

  case 9:                 // MP7 / MP5-SD
    result.push_back(33); // MP7
    result.push_back(23); // MP5-SD
    break;

  case 10: // UMP-45
    result.push_back(24);
    break;

  case 11: // P90
    result.push_back(19);
    break;

  case 12: // PP-Bizon
    result.push_back(26);
    break;

  case 14:                // FAMAS / Galil AR
    result.push_back(10); // FAMAS
    result.push_back(13); // Galil AR
    break;

  case 15:                // AK-47 / M4A4 / M4A1-S
    result.push_back(7);  // AK-47
    result.push_back(16); // M4A4
    result.push_back(60); // M4A1-S
    break;

  case 16: // SSG 08 (Scout)
    result.push_back(40);
    break;

  case 17:                // SG 553 / AUG
    result.push_back(39); // SG 553
    result.push_back(8);  // AUG
    break;

  case 18: // AWP
    result.push_back(9);
    break;

  case 19:                // G3SG1 / SCAR-20
    result.push_back(11); // G3SG1
    result.push_back(38); // SCAR-20
    break;

  case 20: // Nova
    result.push_back(35);
    break;

  case 21: // XM1014
    result.push_back(25);
    break;

  case 22:                // Sawed-Off / MAG-7
    result.push_back(29); // Sawed-Off
    result.push_back(27); // MAG-7
    break;

  case 23: // M249
    result.push_back(14);
    break;

  case 24: // Negev
    result.push_back(28);
    break;

  case 54: // Music kits
    result.push_back(1314);
    break;

  case 55: // Collectibles (Generic catch-all)
    // bruh
    for (uint32_t i = 1000; i <= 5000; i++) {
      result.push_back(i);
    }
    break;
  }

  return result;
}

/**
 * Sends the CMsgSOCacheSubscribed message to a client
 * Populates and sends the full inventory state including items, equipped
 * states, and player data
 *
 * @param p2psocket The socket to send the cache to
 * @param steamId The steam ID of the player
 * @param inventory_db Database connection to fetch inventory data
 */
void GCNetwork_Inventory::SendSOCache(SNetSocket_t p2psocket, uint64_t steamId,
                                      MYSQL *inventory_db) {
  CMsgSOCacheSubscribed cacheMsg;

  cacheMsg.set_version(InventoryVersion);
  cacheMsg.mutable_owner_soid()->set_type(SoIdTypeSteamId);
  cacheMsg.mutable_owner_soid()->set_id(steamId);

  // CSOEconItem
  {
    CMsgSOCacheSubscribed_SubscribedType *object = cacheMsg.add_objects();
    object->set_type_id(SOTypeItem);

    // everyone gets a nametag
    {
      auto nametag = CSOEconItem();
      nametag.set_id(1);
      nametag.set_account_id(steamId & 0xFFFFFFFF);
      nametag.set_def_index(1200);
      nametag.set_inventory(1);
      nametag.set_level(1);
      nametag.set_quality(0);
      nametag.set_flags(0);
      nametag.set_origin(kEconItemOrigin_Purchased);
      nametag.set_rarity(1);

      object->add_object_data(nametag.SerializeAsString());
    }

    // Inject Operation Coin if enabled
    if (TunablesManager::GetInstance().IsOperationActive()) {
      auto coin = CSOEconItem();
      // Use a unique ID high enough to avoid collision with real items
      // We can use steamId + constant to make it deterministic per user
      uint64_t spoofId =
          0xF000000000000000ULL | (steamId & 0x0FFFFFFFFFFFFFFFULL);

      coin.set_id(spoofId);
      coin.set_account_id(steamId & 0xFFFFFFFF);
      coin.set_def_index(4001); // Operation Wildfire Coin (Sample)
      // Or 874? 4001 is common.
      // Let's use 1316 (Operation Wildfire Coin) or 4354 (Bronze).
      // 874 is Operation Breakout.
      // Tunables could specify this, but let's hardcode a good one.
      // 1021 = Operation Bravo
      // 1316 = Operation Wildfire Access Pass? No, Pass is 1316.
      // Coin is what we want.
      // Operation Wildfire Coin = 4354
      coin.set_def_index(4354);
      coin.set_inventory(2);
      coin.set_level(1);
      coin.set_quality(4);
      coin.set_flags(0);
      coin.set_origin(kEconItemOrigin_Purchased);
      coin.set_rarity(1);

      // Attribute 80 (Killeater Score) triggers the journal/stats?
      // Not strictly necessary for the coin itself to appear.

      object->add_object_data(coin.SerializeAsString());
    }

    // SQL injection safe: using prepared statement to fetch item IDs first
    // and then re-querying or just doing it all in one if we can.
    // However, to keep CreateItemFromDatabaseRow working without a massive
    // refactor, we use the parameterized query for security.
    std::string steamId2 = GCNetwork_Users::SteamID64ToSteamID2(steamId);
    auto stmtOpt = createPreparedStatement(
        inventory_db,
        "SELECT id, item_id, floatval, rarity, quality, tradable, "
        "stattrak, stattrak_kills, "
        "sticker_1, sticker_1_wear, sticker_2, sticker_2_wear, "
        "sticker_3, sticker_3_wear, sticker_4, sticker_4_wear, "
        "sticker_5, sticker_5_wear, nametag, pattern_index, "
        "equipped_ct, equipped_t, acknowledged, acquired_by "
        "FROM csgo_items "
        "WHERE owner_steamid2 = ?");

    if (!stmtOpt) {
      logger::error("SendSOCache: Failed to prepare statement");
      return;
    }

    auto &stmt = *stmtOpt;
    unsigned long steamIdLen = steamId2.length();
    stmt.bindString(0, steamId2.c_str(), &steamIdLen);

    if (!stmt.execute()) {
      logger::error("SendSOCache: MySQL query failed: %s", stmt.error());
      return;
    }

    // Use the validated steamId2 to populate the cache.
    // This avoids a massive refactor of CreateItemFromDatabaseRow.

    MYSQL_STMT *stmtHandle = stmt.handle();
    mysql_stmt_store_result(stmtHandle);

    // Bind results for all 24 columns
    MYSQL_BIND binds[24];
    memset(binds, 0, sizeof(binds));

    char id_buf[21], item_id_buf[256], floatval_buf[32], rarity_buf[11],
        quality_buf[11], tradable_buf[2], stattrak_buf[2],
        stattrak_kills_buf[11];
    char sticker_1_buf[11], sticker_1_wear_buf[32], sticker_2_buf[11],
        sticker_2_wear_buf[32], sticker_3_buf[11], sticker_3_wear_buf[32];
    char sticker_4_buf[11], sticker_4_wear_buf[32], sticker_5_buf[11],
        sticker_5_wear_buf[32], nametag_buf[129], pattern_index_buf[11];
    char equipped_ct_buf[2], equipped_t_buf[2], acknowledged_buf[11],
        acquired_by_buf[32];

    unsigned long id_len, item_id_len, floatval_len, rarity_len, quality_len,
        tradable_len, stattrak_len, stattrak_kills_len;
    unsigned long sticker_1_len, sticker_1_wear_len, sticker_2_len,
        sticker_2_wear_len, sticker_3_len, sticker_3_wear_len;
    unsigned long sticker_4_len, sticker_4_wear_len, sticker_5_len,
        sticker_5_wear_len, nametag_len, pattern_index_len;
    unsigned long equipped_ct_len, equipped_t_len, acknowledged_len,
        acquired_by_len;

    my_bool id_is_null, item_id_is_null, floatval_is_null, rarity_is_null,
        quality_is_null, tradable_is_null, stattrak_is_null,
        stattrak_kills_is_null;
    my_bool sticker_1_is_null, sticker_1_wear_is_null, sticker_2_is_null,
        sticker_2_wear_is_null, sticker_3_is_null, sticker_3_wear_is_null;
    my_bool sticker_4_is_null, sticker_4_wear_is_null, sticker_5_is_null,
        sticker_5_wear_is_null, nametag_is_null, pattern_index_is_null;
    my_bool equipped_ct_is_null, equipped_t_is_null, acknowledged_is_null,
        acquired_by_is_null;

    binds[0].buffer_type = MYSQL_TYPE_STRING;
    binds[0].buffer = id_buf;
    binds[0].buffer_length = sizeof(id_buf);
    binds[0].length = &id_len;
    binds[0].is_null = &id_is_null;
    binds[1].buffer_type = MYSQL_TYPE_STRING;
    binds[1].buffer = item_id_buf;
    binds[1].buffer_length = sizeof(item_id_buf);
    binds[1].length = &item_id_len;
    binds[1].is_null = &item_id_is_null;
    binds[2].buffer_type = MYSQL_TYPE_STRING;
    binds[2].buffer = floatval_buf;
    binds[2].buffer_length = sizeof(floatval_buf);
    binds[2].length = &floatval_len;
    binds[2].is_null = &floatval_is_null;
    binds[3].buffer_type = MYSQL_TYPE_STRING;
    binds[3].buffer = rarity_buf;
    binds[3].buffer_length = sizeof(rarity_buf);
    binds[3].length = &rarity_len;
    binds[3].is_null = &rarity_is_null;
    binds[4].buffer_type = MYSQL_TYPE_STRING;
    binds[4].buffer = quality_buf;
    binds[4].buffer_length = sizeof(quality_buf);
    binds[4].length = &quality_len;
    binds[4].is_null = &quality_is_null;
    binds[5].buffer_type = MYSQL_TYPE_STRING;
    binds[5].buffer = tradable_buf;
    binds[5].buffer_length = sizeof(tradable_buf);
    binds[5].length = &tradable_len;
    binds[5].is_null = &tradable_is_null;
    binds[6].buffer_type = MYSQL_TYPE_STRING;
    binds[6].buffer = stattrak_buf;
    binds[6].buffer_length = sizeof(stattrak_buf);
    binds[6].length = &stattrak_len;
    binds[6].is_null = &stattrak_is_null;
    binds[7].buffer_type = MYSQL_TYPE_STRING;
    binds[7].buffer = stattrak_kills_buf;
    binds[7].buffer_length = sizeof(stattrak_kills_buf);
    binds[7].length = &stattrak_kills_len;
    binds[7].is_null = &stattrak_kills_is_null;
    binds[8].buffer_type = MYSQL_TYPE_STRING;
    binds[8].buffer = sticker_1_buf;
    binds[8].buffer_length = sizeof(sticker_1_buf);
    binds[8].length = &sticker_1_len;
    binds[8].is_null = &sticker_1_is_null;
    binds[9].buffer_type = MYSQL_TYPE_STRING;
    binds[9].buffer = sticker_1_wear_buf;
    binds[9].buffer_length = sizeof(sticker_1_wear_buf);
    binds[9].length = &sticker_1_wear_len;
    binds[9].is_null = &sticker_1_wear_is_null;
    binds[10].buffer_type = MYSQL_TYPE_STRING;
    binds[10].buffer = sticker_2_buf;
    binds[10].buffer_length = sizeof(sticker_2_buf);
    binds[10].length = &sticker_2_len;
    binds[10].is_null = &sticker_2_is_null;
    binds[11].buffer_type = MYSQL_TYPE_STRING;
    binds[11].buffer = sticker_2_wear_buf;
    binds[11].buffer_length = sizeof(sticker_2_wear_buf);
    binds[11].length = &sticker_2_wear_len;
    binds[11].is_null = &sticker_2_wear_is_null;
    binds[12].buffer_type = MYSQL_TYPE_STRING;
    binds[12].buffer = sticker_3_buf;
    binds[12].buffer_length = sizeof(sticker_3_buf);
    binds[12].length = &sticker_3_len;
    binds[12].is_null = &sticker_3_is_null;
    binds[13].buffer_type = MYSQL_TYPE_STRING;
    binds[13].buffer = sticker_3_wear_buf;
    binds[13].buffer_length = sizeof(sticker_3_wear_buf);
    binds[13].length = &sticker_3_wear_len;
    binds[13].is_null = &sticker_3_wear_is_null;
    binds[14].buffer_type = MYSQL_TYPE_STRING;
    binds[14].buffer = sticker_4_buf;
    binds[14].buffer_length = sizeof(sticker_4_buf);
    binds[14].length = &sticker_4_len;
    binds[14].is_null = &sticker_4_is_null;
    binds[15].buffer_type = MYSQL_TYPE_STRING;
    binds[15].buffer = sticker_4_wear_buf;
    binds[15].buffer_length = sizeof(sticker_4_wear_buf);
    binds[15].length = &sticker_4_wear_len;
    binds[15].is_null = &sticker_4_wear_is_null;
    binds[16].buffer_type = MYSQL_TYPE_STRING;
    binds[16].buffer = sticker_5_buf;
    binds[16].buffer_length = sizeof(sticker_5_buf);
    binds[16].length = &sticker_5_len;
    binds[16].is_null = &sticker_5_is_null;
    binds[17].buffer_type = MYSQL_TYPE_STRING;
    binds[17].buffer = sticker_5_wear_buf;
    binds[17].buffer_length = sizeof(sticker_5_wear_buf);
    binds[17].length = &sticker_5_wear_len;
    binds[17].is_null = &sticker_5_wear_is_null;
    binds[18].buffer_type = MYSQL_TYPE_STRING;
    binds[18].buffer = nametag_buf;
    binds[18].buffer_length = sizeof(nametag_buf);
    binds[18].length = &nametag_len;
    binds[18].is_null = &nametag_is_null;
    binds[19].buffer_type = MYSQL_TYPE_STRING;
    binds[19].buffer = pattern_index_buf;
    binds[19].buffer_length = sizeof(pattern_index_buf);
    binds[19].length = &pattern_index_len;
    binds[19].is_null = &pattern_index_is_null;
    binds[20].buffer_type = MYSQL_TYPE_STRING;
    binds[20].buffer = equipped_ct_buf;
    binds[20].buffer_length = sizeof(equipped_ct_buf);
    binds[20].length = &equipped_ct_len;
    binds[20].is_null = &equipped_ct_is_null;
    binds[21].buffer_type = MYSQL_TYPE_STRING;
    binds[21].buffer = equipped_t_buf;
    binds[21].buffer_length = sizeof(equipped_t_buf);
    binds[21].length = &equipped_t_len;
    binds[21].is_null = &equipped_t_is_null;
    binds[22].buffer_type = MYSQL_TYPE_STRING;
    binds[22].buffer = acknowledged_buf;
    binds[22].buffer_length = sizeof(acknowledged_buf);
    binds[22].length = &acknowledged_len;
    binds[22].is_null = &acknowledged_is_null;
    binds[23].buffer_type = MYSQL_TYPE_STRING;
    binds[23].buffer = acquired_by_buf;
    binds[23].buffer_length = sizeof(acquired_by_buf);
    binds[23].length = &acquired_by_len;
    binds[23].is_null = &acquired_by_is_null;

    if (mysql_stmt_bind_result(stmtHandle, binds)) {
      logger::error(
          "SendSOCache: Failed to bind result for prepared statement: %s",
          mysql_stmt_error(stmtHandle));
      return;
    }

    // Simulate MYSQL_ROW for CreateItemFromDatabaseRow
    const char *row_data[24];
    my_bool row_is_null[24];

    while (mysql_stmt_fetch(stmtHandle) == 0) {
      row_data[0] = id_is_null ? nullptr : id_buf;
      row_data[1] = item_id_is_null ? nullptr : item_id_buf;
      row_data[2] = floatval_is_null ? nullptr : floatval_buf;
      row_data[3] = rarity_is_null ? nullptr : rarity_buf;
      row_data[4] = quality_is_null ? nullptr : quality_buf;
      row_data[5] = tradable_is_null ? nullptr : tradable_buf;
      row_data[6] = stattrak_is_null ? nullptr : stattrak_buf;
      row_data[7] = stattrak_kills_is_null ? nullptr : stattrak_kills_buf;
      row_data[8] = sticker_1_is_null ? nullptr : sticker_1_buf;
      row_data[9] = sticker_1_wear_is_null ? nullptr : sticker_1_wear_buf;
      row_data[10] = sticker_2_is_null ? nullptr : sticker_2_buf;
      row_data[11] = sticker_2_wear_is_null ? nullptr : sticker_2_wear_buf;
      row_data[12] = sticker_3_is_null ? nullptr : sticker_3_buf;
      row_data[13] = sticker_3_wear_is_null ? nullptr : sticker_3_wear_buf;
      row_data[14] = sticker_4_is_null ? nullptr : sticker_4_buf;
      row_data[15] = sticker_4_wear_is_null ? nullptr : sticker_4_wear_buf;
      row_data[16] = sticker_5_is_null ? nullptr : sticker_5_buf;
      row_data[17] = sticker_5_wear_is_null ? nullptr : sticker_5_wear_buf;
      row_data[18] = nametag_is_null ? nullptr : nametag_buf;
      row_data[19] = pattern_index_is_null ? nullptr : pattern_index_buf;
      row_data[20] = equipped_ct_is_null ? nullptr : equipped_ct_buf;
      row_data[21] = equipped_t_is_null ? nullptr : equipped_t_buf;
      row_data[22] = acknowledged_is_null ? nullptr : acknowledged_buf;
      row_data[23] = acquired_by_is_null ? nullptr : acquired_by_buf;

      if (!row_data[1]) {
        logger::error("SendSOCache: Item ID is NULL in database row");
        continue;
      }

      try {
        auto item = CreateItemFromDatabaseRow(steamId, (MYSQL_ROW)row_data);
        if (item) {
          object->add_object_data(item->SerializeAsString());
          // Smart pointer automatically cleans up
        }
      } catch (const std::exception &e) {
        logger::error("SendSOCache: Exception while processing item: %s",
                      e.what());
        continue;
      }
    }

    mysql_stmt_free_result(stmtHandle);
  }

  // SOTypeDefaultEquippedDefinitionInstanceClient
  {
    // Ensure the row exists - SQL injection safe
    auto insertStmtOpt = createPreparedStatement(
        inventory_db,
        "INSERT IGNORE INTO csgo_defaultequips (owner_id) VALUES (?)");

    if (!insertStmtOpt) {
      logger::error(
          "SendSOCache: Failed to prepare defaultequips insert check");
    } else {
      auto &stmt = *insertStmtOpt;
      uint64_t ownerParam = steamId;
      stmt.bindUint64(0, &ownerParam);
      if (!stmt.execute()) {
        logger::error(
            "SendSOCache: MySQL default equips insert check failed: %s",
            stmt.error());
      }
    }

    // Query default equips - SQL injection safe
    auto selectStmtOpt = createPreparedStatement(
        inventory_db, "SELECT default_usp_ct, default_m4a1s_ct, default_r8_ct, "
                      "default_r8_t, default_cz75_ct, default_cz75_t "
                      "FROM csgo_defaultequips WHERE owner_id = ?");

    if (!selectStmtOpt) {
      logger::error("SendSOCache: Failed to prepare default equips select");
      return;
    }

    auto &selectStmt = *selectStmtOpt;
    uint64_t ownerForSelect = steamId;
    selectStmt.bindUint64(0, &ownerForSelect);

    if (!selectStmt.execute() || !selectStmt.storeResult() ||
        selectStmt.numRows() == 0) {
      logger::warning(
          "SendSOCache: No default equips row found for player %llu", steamId);
      return;
    }

    // Bind results for the 6 columns
    int usp_ct, m4_ct, r8_ct, r8_t, cz_ct, cz_t;
    my_bool nulls[6];
    MYSQL_BIND binds[6];
    memset(binds, 0, sizeof(binds));

    for (int i = 0; i < 6; ++i) {
      binds[i].buffer_type = MYSQL_TYPE_LONG;
      binds[i].is_null = &nulls[i];
    }
    binds[0].buffer = &usp_ct;
    binds[1].buffer = &m4_ct;
    binds[2].buffer = &r8_ct;
    binds[3].buffer = &r8_t;
    binds[4].buffer = &cz_ct;
    binds[5].buffer = &cz_t;

    if (!selectStmt.bindResult(binds) || selectStmt.fetch() != 0) {
      logger::error("SendSOCache: Failed to fetch default equips");
      return;
    }

    {
      CMsgSOCacheSubscribed_SubscribedType *object = cacheMsg.add_objects();
      object->set_type_id(SOTypeDefaultEquippedDefinitionInstanceClient);

      uint32_t account_id = steamId & 0xFFFFFFFF;

      // USP-S for CT (def_index 61, slot 2)
      if (!nulls[0] && usp_ct == 1) {
        CSOEconDefaultEquippedDefinitionInstanceClient defaultEquip;
        defaultEquip.set_account_id(account_id);
        defaultEquip.set_item_definition(61);
        defaultEquip.set_class_id(CLASS_CT);
        defaultEquip.set_slot_id(2);
        object->add_object_data(defaultEquip.SerializeAsString());
      }

      // M4A1-S for CT (def_index 60, slot 15)
      if (!nulls[1] && m4_ct == 1) {
        CSOEconDefaultEquippedDefinitionInstanceClient defaultEquip;
        defaultEquip.set_account_id(account_id);
        defaultEquip.set_item_definition(60);
        defaultEquip.set_class_id(CLASS_CT);
        defaultEquip.set_slot_id(15);
        object->add_object_data(defaultEquip.SerializeAsString());
      }

      // R8 Revolver
      if (!nulls[2] && r8_ct == 1) // default_r8_ct
      {
        CSOEconDefaultEquippedDefinitionInstanceClient defaultEquip;
        defaultEquip.set_account_id(account_id);
        defaultEquip.set_item_definition(64);
        defaultEquip.set_class_id(CLASS_CT);
        defaultEquip.set_slot_id(6);
        object->add_object_data(defaultEquip.SerializeAsString());
      }

      if (!nulls[3] && r8_t == 1) // default_r8_t
      {
        CSOEconDefaultEquippedDefinitionInstanceClient defaultEquip;
        defaultEquip.set_account_id(account_id);
        defaultEquip.set_item_definition(64);
        defaultEquip.set_class_id(CLASS_T);
        defaultEquip.set_slot_id(6);
        object->add_object_data(defaultEquip.SerializeAsString());
      }

      // CZ75-Auto
      if (!nulls[4] && cz_ct == 1) // default_cz75_ct
      {
        CSOEconDefaultEquippedDefinitionInstanceClient defaultEquip;
        defaultEquip.set_account_id(account_id);
        defaultEquip.set_item_definition(63);
        defaultEquip.set_class_id(CLASS_CT);
        defaultEquip.set_slot_id(5);
        object->add_object_data(defaultEquip.SerializeAsString());
      }

      if (!nulls[5] && cz_t == 1) // default_cz75_t
      {
        CSOEconDefaultEquippedDefinitionInstanceClient defaultEquip;
        defaultEquip.set_account_id(account_id);
        defaultEquip.set_item_definition(63);
        defaultEquip.set_class_id(CLASS_T);
        defaultEquip.set_slot_id(5);
        object->add_object_data(defaultEquip.SerializeAsString());
      }
    }
  }

  // PersonaData
  {
    CSOPersonaDataPublic personaData;
    personaData.set_player_level(1); // todo: fetch from db
    personaData.set_elevated_state(true);

    CMsgSOCacheSubscribed_SubscribedType *object = cacheMsg.add_objects();
    object->set_type_id(SOTypePersonaDataPublic);
    object->add_object_data(personaData.SerializeAsString());
  }

  // GameAccountClient (if (!server))
  {
    CSOEconGameAccountClient accountClient;
    accountClient.set_additional_backpack_slots(0);
    accountClient.set_bonus_xp_timestamp_refresh(
        static_cast<uint32_t>(time(nullptr)));
    accountClient.set_bonus_xp_usedflags(
        16); // caught cheater lobbies, overwatch bonus etc
    accountClient.set_elevated_state(ElevatedStatePrime);
    accountClient.set_elevated_timestamp(
        ElevatedStatePrime); // is this actually 5???

    CMsgSOCacheSubscribed_SubscribedType *object = cacheMsg.add_objects();
    object->set_type_id(SOTypeGameAccountClient);
    object->add_object_data(accountClient.SerializeAsString());
  }

  NetworkMessage responseMsg =
      NetworkMessage::FromProto(cacheMsg, k_EMsgGC_CC_GC2CL_SOCacheSubscribed);

  // Log total objects and serialized size of cached objects
  logger::info("SendSOCache: Sending SOCache - Total objects: %d",
               cacheMsg.objects_size());

  // Log individual object details
  for (int i = 0; i < cacheMsg.objects_size(); i++) {
    const auto &obj = cacheMsg.objects(i);
    logger::info("Object %d - Type: %u, Data count: %d, Object size: %d", i,
                 obj.type_id(), obj.object_data_size(), obj.ByteSizeLong());
  }

  uint32_t totalSize = responseMsg.GetTotalSize();
  logger::info("SendSOCache: Total message size: %u bytes", totalSize);

  responseMsg.WriteToSocket(p2psocket, true);

  logger::info("SendSOCache: Sent SOCache for steamid %llu", steamId);
}

/**
 * Helper function to create a fully populated CSOEconItem from database row
 *
 * @param steamId The steam ID of the item's owner
 * @param row MySQL row containing the item data
 * @param overrideAcknowledged Optional value to override the
 * acknowledged/inventory position
 * @return Pointer to a new CSOEconItem object (caller must manage memory)
 */
std::unique_ptr<CSOEconItem>
GCNetwork_Inventory::CreateItemFromDatabaseRow(uint64_t steamId, MYSQL_ROW row,
                                               int overrideAcknowledged) {
  if (!row) {
    logger::error("CreateItemFromDatabaseRow: NULL row pointer");
    return nullptr;
  }

  try {
    auto item = std::make_unique<CSOEconItem>();

    // Parse item_id and get def_index and paint_index
    uint32_t def_index, paint_index;
    if (!row[1] || !ParseItemId(row[1], def_index, paint_index)) {
      logger::error("CreateItemFromDatabaseRow: Failed to parse item_id: %s",
                    row[1] ? row[1] : "null");
      return nullptr;
    }

    // Base properties
    item->set_id(row[0] ? strtoull(row[0], nullptr, 10) : 0);
    item->set_account_id(steamId & 0xFFFFFFFF);
    item->set_def_index(def_index);

    // Set inventory position (acknowledged)
    if (overrideAcknowledged >= 0) {
      item->set_inventory(overrideAcknowledged);
    } else {
      item->set_inventory(SafeParse::toUint32(row[22]).value_or(0));
    }

    item->set_level(1);
    item->set_quality(SafeParse::toUint32(row[4]).value_or(0));
    item->set_flags(0);

    // Item origin
    int originType = kEconItemOrigin_FoundInCrate;
    if (row[23]) {
      std::string acquiredBy = row[23];
      if (acquiredBy == "trade") {
        originType = kEconItemOrigin_Traded;
      } else if (acquiredBy == "trade_up") {
        originType = kEconItemOrigin_Crafted;
      } else if (acquiredBy == "ingame_drop") {
        originType = kEconItemOrigin_Drop;
      } else if (acquiredBy == "purchased") {
        originType = kEconItemOrigin_Purchased;
      } else if (acquiredBy == "0" || acquiredBy.empty()) {
        originType = kEconItemOrigin_FoundInCrate;
      }
    }
    item->set_origin(originType);

    // Custom name
    if (row[18] && strlen(row[18]) > 0) {
      item->set_custom_name(row[18]);
    }

    // Rarity (add 1 to match expected range)
    item->set_rarity(SafeParse::toInt(row[3]).value_or(-1) + 1);

    // Set attributes based on item type
    if (def_index == 1209) {
      // Sticker item
      AddUint32Attribute(item.get(), ATTR_ITEM_STICKER_ID, paint_index);
    } else if (def_index == 1314) {
      // Music kit item
      AddUint32Attribute(item.get(), ATTR_ITEM_MUSICKIT_ID, paint_index);
    } else {
      // Weapon skins
      if (paint_index > 0) {
        AddFloatAttribute(item.get(), ATTR_PAINT_INDEX, paint_index);

        if (row[2] && strlen(row[2]) > 0) {
          AddFloatAttribute(item.get(), ATTR_PAINT_WEAR, std::stof(row[2]));
        }

        if (row[19] && strlen(row[19]) > 0) {
          AddFloatAttribute(item.get(), ATTR_PAINT_SEED,
                            SafeParse::toInt(row[19]).value_or(0));
        }
      }

      // StatTrak
      if (SafeParse::toInt(row[6]).value_or(0) == 1) {
        AddUint32Attribute(item.get(), ATTR_KILLEATER_SCORE,
                           SafeParse::toInt(row[7]).value_or(0));
        AddUint32Attribute(item.get(), ATTR_KILLEATER_TYPE, 0);
      }

      // Untradable
      if (SafeParse::toInt(row[5]).value_or(1) == 0) {
        AddUint32Attribute(item.get(), ATTR_TRADE_RESTRICTION,
                           3133696800); // 4/20/2069
      }

      // Stickers for weapons only
      if (def_index != 1209 && def_index != 1314) {
        for (int i = 0; i < 5; i++) {
          AddStickerAttributes(item.get(), row, i);
        }
      }
    }

    // Equipment state
    bool equipped_ct = SafeParse::toInt(row[20]).value_or(0) == 1;
    bool equipped_t = SafeParse::toInt(row[21]).value_or(0) == 1;

    std::string item_id = row[1] ? row[1] : "";
    bool isCollectible = (item_id.length() >= 12)
                             ? (item_id.substr(0, 12) == "collectible-")
                             : false;
    bool isMusicKit = (def_index == 1314);

    if (isCollectible || isMusicKit) {
      if (equipped_t) {
        AddEquippedState(item.get(), true, 0, def_index);
      }
    }
    // Weapon skins (not stickers)
    else if (def_index != 1209) {
      AddEquippedState(item.get(), equipped_ct, CLASS_CT, def_index);
      AddEquippedState(item.get(), equipped_t, CLASS_T, def_index);
    }

    return item;
  } catch (const std::exception &e) {
    logger::error("CreateItemFromDatabaseRow: Exception caught: %s", e.what());
    return nullptr;
  } catch (...) {
    logger::error("CreateItemFromDatabaseRow: Unknown exception caught");
    return nullptr;
  }
}

/**
 * Helper to fetch an item from database and create a CSOEconItem
 *
 * @param itemId The unique ID of the item
 * @param steamId The owner's Steam ID
 * @param inventory_db Database connection
 * @param overrideAcknowledged Optional value to override the
 * acknowledged/inventory position
 * @return Pointer to a new CSOEconItem object (caller must manage memory)
 */
std::unique_ptr<CSOEconItem>
GCNetwork_Inventory::FetchItemFromDatabase(uint64_t itemId, uint64_t steamId,
                                           MYSQL *inventory_db,
                                           int overrideAcknowledged) {
  if (!inventory_db) {
    logger::error("FetchItemFromDatabase: NULL database connection");
    return nullptr;
  }

  // SQL injection safe: using prepared statement
  std::string steamId2 = GCNetwork_Users::SteamID64ToSteamID2(steamId);

  auto stmtOpt = createPreparedStatement(
      inventory_db, "SELECT id, item_id, floatval, rarity, quality, tradable, "
                    "stattrak, stattrak_kills, "
                    "sticker_1, sticker_1_wear, sticker_2, sticker_2_wear, "
                    "sticker_3, sticker_3_wear, sticker_4, sticker_4_wear, "
                    "sticker_5, sticker_5_wear, nametag, pattern_index, "
                    "equipped_ct, equipped_t, acknowledged, acquired_by "
                    "FROM csgo_items "
                    "WHERE id = ? AND owner_steamid2 = ?");

  if (!stmtOpt) {
    logger::error("FetchItemFromDatabase: Failed to prepare statement");
    return nullptr;
  }

  auto &stmt = *stmtOpt;
  uint64_t itemIdParam = itemId;
  unsigned long steamIdLen = steamId2.length();
  stmt.bindUint64(0, &itemIdParam);
  stmt.bindString(1, steamId2.c_str(), &steamIdLen);

  if (!stmt.execute()) {
    logger::error("FetchItemFromDatabase: MySQL query failed: %s",
                  stmt.error());
    return nullptr;
  }

  // Use the validated itemId and steamId2 to fetch the item.
  // This avoids a massive refactor of CreateItemFromDatabaseRow.

  MYSQL_STMT *stmtHandle = stmt.handle();
  mysql_stmt_store_result(stmtHandle);

  // Bind results for all 24 columns
  MYSQL_BIND binds[24];
  memset(binds, 0, sizeof(binds));

  char id_buf[21], item_id_buf[256], floatval_buf[32], rarity_buf[11],
      quality_buf[11], tradable_buf[2], stattrak_buf[2], stattrak_kills_buf[11];
  char sticker_1_buf[11], sticker_1_wear_buf[32], sticker_2_buf[11],
      sticker_2_wear_buf[32], sticker_3_buf[11], sticker_3_wear_buf[32];
  char sticker_4_buf[11], sticker_4_wear_buf[32], sticker_5_buf[11],
      sticker_5_wear_buf[32], nametag_buf[129], pattern_index_buf[11];
  char equipped_ct_buf[2], equipped_t_buf[2], acknowledged_buf[11],
      acquired_by_buf[32];

  unsigned long id_len, item_id_len, floatval_len, rarity_len, quality_len,
      tradable_len, stattrak_len, stattrak_kills_len;
  unsigned long sticker_1_len, sticker_1_wear_len, sticker_2_len,
      sticker_2_wear_len, sticker_3_len, sticker_3_wear_len;
  unsigned long sticker_4_len, sticker_4_wear_len, sticker_5_len,
      sticker_5_wear_len, nametag_len, pattern_index_len;
  unsigned long equipped_ct_len, equipped_t_len, acknowledged_len,
      acquired_by_len;

  my_bool id_is_null, item_id_is_null, floatval_is_null, rarity_is_null,
      quality_is_null, tradable_is_null, stattrak_is_null,
      stattrak_kills_is_null;
  my_bool sticker_1_is_null, sticker_1_wear_is_null, sticker_2_is_null,
      sticker_2_wear_is_null, sticker_3_is_null, sticker_3_wear_is_null;
  my_bool sticker_4_is_null, sticker_4_wear_is_null, sticker_5_is_null,
      sticker_5_wear_is_null, nametag_is_null, pattern_index_is_null;
  my_bool equipped_ct_is_null, equipped_t_is_null, acknowledged_is_null,
      acquired_by_is_null;

  binds[0].buffer_type = MYSQL_TYPE_STRING;
  binds[0].buffer = id_buf;
  binds[0].buffer_length = sizeof(id_buf);
  binds[0].length = &id_len;
  binds[0].is_null = &id_is_null;
  binds[1].buffer_type = MYSQL_TYPE_STRING;
  binds[1].buffer = item_id_buf;
  binds[1].buffer_length = sizeof(item_id_buf);
  binds[1].length = &item_id_len;
  binds[1].is_null = &item_id_is_null;
  binds[2].buffer_type = MYSQL_TYPE_STRING;
  binds[2].buffer = floatval_buf;
  binds[2].buffer_length = sizeof(floatval_buf);
  binds[2].length = &floatval_len;
  binds[2].is_null = &floatval_is_null;
  binds[3].buffer_type = MYSQL_TYPE_STRING;
  binds[3].buffer = rarity_buf;
  binds[3].buffer_length = sizeof(rarity_buf);
  binds[3].length = &rarity_len;
  binds[3].is_null = &rarity_is_null;
  binds[4].buffer_type = MYSQL_TYPE_STRING;
  binds[4].buffer = quality_buf;
  binds[4].buffer_length = sizeof(quality_buf);
  binds[4].length = &quality_len;
  binds[4].is_null = &quality_is_null;
  binds[5].buffer_type = MYSQL_TYPE_STRING;
  binds[5].buffer = tradable_buf;
  binds[5].buffer_length = sizeof(tradable_buf);
  binds[5].length = &tradable_len;
  binds[5].is_null = &tradable_is_null;
  binds[6].buffer_type = MYSQL_TYPE_STRING;
  binds[6].buffer = stattrak_buf;
  binds[6].buffer_length = sizeof(stattrak_buf);
  binds[6].length = &stattrak_len;
  binds[6].is_null = &stattrak_is_null;
  binds[7].buffer_type = MYSQL_TYPE_STRING;
  binds[7].buffer = stattrak_kills_buf;
  binds[7].buffer_length = sizeof(stattrak_kills_buf);
  binds[7].length = &stattrak_kills_len;
  binds[7].is_null = &stattrak_kills_is_null;
  binds[8].buffer_type = MYSQL_TYPE_STRING;
  binds[8].buffer = sticker_1_buf;
  binds[8].buffer_length = sizeof(sticker_1_buf);
  binds[8].length = &sticker_1_len;
  binds[8].is_null = &sticker_1_is_null;
  binds[9].buffer_type = MYSQL_TYPE_STRING;
  binds[9].buffer = sticker_1_wear_buf;
  binds[9].buffer_length = sizeof(sticker_1_wear_buf);
  binds[9].length = &sticker_1_wear_len;
  binds[9].is_null = &sticker_1_wear_is_null;
  binds[10].buffer_type = MYSQL_TYPE_STRING;
  binds[10].buffer = sticker_2_buf;
  binds[10].buffer_length = sizeof(sticker_2_buf);
  binds[10].length = &sticker_2_len;
  binds[10].is_null = &sticker_2_is_null;
  binds[11].buffer_type = MYSQL_TYPE_STRING;
  binds[11].buffer = sticker_2_wear_buf;
  binds[11].buffer_length = sizeof(sticker_2_wear_buf);
  binds[11].length = &sticker_2_wear_len;
  binds[11].is_null = &sticker_2_wear_is_null;
  binds[12].buffer_type = MYSQL_TYPE_STRING;
  binds[12].buffer = sticker_3_buf;
  binds[12].buffer_length = sizeof(sticker_3_buf);
  binds[12].length = &sticker_3_len;
  binds[12].is_null = &sticker_3_is_null;
  binds[13].buffer_type = MYSQL_TYPE_STRING;
  binds[13].buffer = sticker_3_wear_buf;
  binds[13].buffer_length = sizeof(sticker_3_wear_buf);
  binds[13].length = &sticker_3_wear_len;
  binds[13].is_null = &sticker_3_wear_is_null;
  binds[14].buffer_type = MYSQL_TYPE_STRING;
  binds[14].buffer = sticker_4_buf;
  binds[14].buffer_length = sizeof(sticker_4_buf);
  binds[14].length = &sticker_4_len;
  binds[14].is_null = &sticker_4_is_null;
  binds[15].buffer_type = MYSQL_TYPE_STRING;
  binds[15].buffer = sticker_4_wear_buf;
  binds[15].buffer_length = sizeof(sticker_4_wear_buf);
  binds[15].length = &sticker_4_wear_len;
  binds[15].is_null = &sticker_4_wear_is_null;
  binds[16].buffer_type = MYSQL_TYPE_STRING;
  binds[16].buffer = sticker_5_buf;
  binds[16].buffer_length = sizeof(sticker_5_buf);
  binds[16].length = &sticker_5_len;
  binds[16].is_null = &sticker_5_is_null;
  binds[17].buffer_type = MYSQL_TYPE_STRING;
  binds[17].buffer = sticker_5_wear_buf;
  binds[17].buffer_length = sizeof(sticker_5_wear_buf);
  binds[17].length = &sticker_5_wear_len;
  binds[17].is_null = &sticker_5_wear_is_null;
  binds[18].buffer_type = MYSQL_TYPE_STRING;
  binds[18].buffer = nametag_buf;
  binds[18].buffer_length = sizeof(nametag_buf);
  binds[18].length = &nametag_len;
  binds[18].is_null = &nametag_is_null;
  binds[19].buffer_type = MYSQL_TYPE_STRING;
  binds[19].buffer = pattern_index_buf;
  binds[19].buffer_length = sizeof(pattern_index_buf);
  binds[19].length = &pattern_index_len;
  binds[19].is_null = &pattern_index_is_null;
  binds[20].buffer_type = MYSQL_TYPE_STRING;
  binds[20].buffer = equipped_ct_buf;
  binds[20].buffer_length = sizeof(equipped_ct_buf);
  binds[20].length = &equipped_ct_len;
  binds[20].is_null = &equipped_ct_is_null;
  binds[21].buffer_type = MYSQL_TYPE_STRING;
  binds[21].buffer = equipped_t_buf;
  binds[21].buffer_length = sizeof(equipped_t_buf);
  binds[21].length = &equipped_t_len;
  binds[21].is_null = &equipped_t_is_null;
  binds[22].buffer_type = MYSQL_TYPE_STRING;
  binds[22].buffer = acknowledged_buf;
  binds[22].buffer_length = sizeof(acknowledged_buf);
  binds[22].length = &acknowledged_len;
  binds[22].is_null = &acknowledged_is_null;
  binds[23].buffer_type = MYSQL_TYPE_STRING;
  binds[23].buffer = acquired_by_buf;
  binds[23].buffer_length = sizeof(acquired_by_buf);
  binds[23].length = &acquired_by_len;
  binds[23].is_null = &acquired_by_is_null;

  if (mysql_stmt_bind_result(stmtHandle, binds)) {
    logger::error("FetchItemFromDatabase: Failed to bind result for prepared "
                  "statement: %s",
                  mysql_stmt_error(stmtHandle));
    return nullptr;
  }

  std::unique_ptr<CSOEconItem> item = nullptr;
  if (mysql_stmt_fetch(stmtHandle) == 0) {
    const char *row_data[24];
    row_data[0] = id_is_null ? nullptr : id_buf;
    row_data[1] = item_id_is_null ? nullptr : item_id_buf;
    row_data[2] = floatval_is_null ? nullptr : floatval_buf;
    row_data[3] = rarity_is_null ? nullptr : rarity_buf;
    row_data[4] = quality_is_null ? nullptr : quality_buf;
    row_data[5] = tradable_is_null ? nullptr : tradable_buf;
    row_data[6] = stattrak_is_null ? nullptr : stattrak_buf;
    row_data[7] = stattrak_kills_is_null ? nullptr : stattrak_kills_buf;
    row_data[8] = sticker_1_is_null ? nullptr : sticker_1_buf;
    row_data[9] = sticker_1_wear_is_null ? nullptr : sticker_1_wear_buf;
    row_data[10] = sticker_2_is_null ? nullptr : sticker_2_buf;
    row_data[11] = sticker_2_wear_is_null ? nullptr : sticker_2_wear_buf;
    row_data[12] = sticker_3_is_null ? nullptr : sticker_3_buf;
    row_data[13] = sticker_3_wear_is_null ? nullptr : sticker_3_wear_buf;
    row_data[14] = sticker_4_is_null ? nullptr : sticker_4_buf;
    row_data[15] = sticker_4_wear_is_null ? nullptr : sticker_4_wear_buf;
    row_data[16] = sticker_5_is_null ? nullptr : sticker_5_buf;
    row_data[17] = sticker_5_wear_is_null ? nullptr : sticker_5_wear_buf;
    row_data[18] = nametag_is_null ? nullptr : nametag_buf;
    row_data[19] = pattern_index_is_null ? nullptr : pattern_index_buf;
    row_data[20] = equipped_ct_is_null ? nullptr : equipped_ct_buf;
    row_data[21] = equipped_t_is_null ? nullptr : equipped_t_buf;
    row_data[22] = acknowledged_is_null ? nullptr : acknowledged_buf;
    row_data[23] = acquired_by_is_null ? nullptr : acquired_by_buf;

    item = CreateItemFromDatabaseRow(steamId, (MYSQL_ROW)row_data,
                                     overrideAcknowledged);
  } else {
    logger::error("FetchItemFromDatabase: Item not found: %llu", itemId);
  }

  mysql_stmt_free_result(stmtHandle);
  return item;
}

/**
 * Checks for new items and sends them to the client
 * For items with acquired_by="0", also sends the same item as an
 * UnlockCrateResponse
 *
 * @param p2psocket The socket to send updates to
 * @param steamId The steam ID of the player
 * @param lastItemId Reference to the last known item ID, which will be updated
 * @param inventory_db Database connection to fetch inventory data
 * @return True if new items were found and sent
 */
bool GCNetwork_Inventory::CheckAndSendNewItemsSince(SNetSocket_t p2psocket,
                                                    uint64_t steamId,
                                                    uint64_t &lastItemId,
                                                    MYSQL *inventory_db) {
  if (!inventory_db) {
    logger::error("CheckAndSendNewItemsSince: Database connection is null");
    return false;
  }

  // SQL injection safe: using prepared statement to validate and query
  std::string steamId2 = GCNetwork_Users::SteamID64ToSteamID2(steamId);
  auto stmtOpt = createPreparedStatement(
      inventory_db, "SELECT id, item_id, floatval, rarity, quality, tradable, "
                    "stattrak, stattrak_kills, "
                    "sticker_1, sticker_1_wear, sticker_2, sticker_2_wear, "
                    "sticker_3, sticker_3_wear, sticker_4, sticker_4_wear, "
                    "sticker_5, sticker_5_wear, nametag, pattern_index, "
                    "equipped_ct, equipped_t, acknowledged, acquired_by "
                    "FROM csgo_items "
                    "WHERE owner_steamid2 = ? AND id > ? "
                    "ORDER BY id ASC");

  if (!stmtOpt) {
    logger::error("CheckAndSendNewItemsSince: Failed to prepare statement");
    return false;
  }

  auto &stmt = *stmtOpt;
  uint64_t lastIdParam = lastItemId;
  unsigned long steamIdLen = steamId2.length();
  stmt.bindString(0, steamId2.c_str(), &steamIdLen);
  stmt.bindUint64(1, &lastIdParam);

  if (!stmt.execute()) {
    logger::error("CheckAndSendNewItemsSince: MySQL query failed: %s",
                  stmt.error());
    return false;
  }

  // Use the validated steamId2 and lastItemId to fetch new items.
  // This avoids a massive refactor of the complex loop below.

  MYSQL_STMT *stmtHandle = stmt.handle();
  mysql_stmt_store_result(stmtHandle);

  int numRows = mysql_stmt_num_rows(stmtHandle);
  if (numRows == 0) {
    // no items
    mysql_stmt_free_result(stmtHandle);
    return false;
  }

  logger::info("CheckAndSendNewItemsSince: Found %d new items for player %llu",
               numRows, steamId);

  bool updateSuccess = false;
  uint64_t highestItemId = lastItemId;

  // Bind results for all 24 columns
  MYSQL_BIND binds[24];
  memset(binds, 0, sizeof(binds));

  char id_buf[21], item_id_buf[256], floatval_buf[32], rarity_buf[11],
      quality_buf[11], tradable_buf[2], stattrak_buf[2], stattrak_kills_buf[11];
  char sticker_1_buf[11], sticker_1_wear_buf[32], sticker_2_buf[11],
      sticker_2_wear_buf[32], sticker_3_buf[11], sticker_3_wear_buf[32];
  char sticker_4_buf[11], sticker_4_wear_buf[32], sticker_5_buf[11],
      sticker_5_wear_buf[32], nametag_buf[129], pattern_index_buf[11];
  char equipped_ct_buf[2], equipped_t_buf[2], acknowledged_buf[11],
      acquired_by_buf[32];

  unsigned long id_len, item_id_len, floatval_len, rarity_len, quality_len,
      tradable_len, stattrak_len, stattrak_kills_len;
  unsigned long sticker_1_len, sticker_1_wear_len, sticker_2_len,
      sticker_2_wear_len, sticker_3_len, sticker_3_wear_len;
  unsigned long sticker_4_len, sticker_4_wear_len, sticker_5_len,
      sticker_5_wear_len, nametag_len, pattern_index_len;
  unsigned long equipped_ct_len, equipped_t_len, acknowledged_len,
      acquired_by_len;

  my_bool id_is_null, item_id_is_null, floatval_is_null, rarity_is_null,
      quality_is_null, tradable_is_null, stattrak_is_null,
      stattrak_kills_is_null;
  my_bool sticker_1_is_null, sticker_1_wear_is_null, sticker_2_is_null,
      sticker_2_wear_is_null, sticker_3_is_null, sticker_3_wear_is_null;
  my_bool sticker_4_is_null, sticker_4_wear_is_null, sticker_5_is_null,
      sticker_5_wear_is_null, nametag_is_null, pattern_index_is_null;
  my_bool equipped_ct_is_null, equipped_t_is_null, acknowledged_is_null,
      acquired_by_is_null;

  binds[0].buffer_type = MYSQL_TYPE_STRING;
  binds[0].buffer = id_buf;
  binds[0].buffer_length = sizeof(id_buf);
  binds[0].length = &id_len;
  binds[0].is_null = &id_is_null;
  binds[1].buffer_type = MYSQL_TYPE_STRING;
  binds[1].buffer = item_id_buf;
  binds[1].buffer_length = sizeof(item_id_buf);
  binds[1].length = &item_id_len;
  binds[1].is_null = &item_id_is_null;
  binds[2].buffer_type = MYSQL_TYPE_STRING;
  binds[2].buffer = floatval_buf;
  binds[2].buffer_length = sizeof(floatval_buf);
  binds[2].length = &floatval_len;
  binds[2].is_null = &floatval_is_null;
  binds[3].buffer_type = MYSQL_TYPE_STRING;
  binds[3].buffer = rarity_buf;
  binds[3].buffer_length = sizeof(rarity_buf);
  binds[3].length = &rarity_len;
  binds[3].is_null = &rarity_is_null;
  binds[4].buffer_type = MYSQL_TYPE_STRING;
  binds[4].buffer = quality_buf;
  binds[4].buffer_length = sizeof(quality_buf);
  binds[4].length = &quality_len;
  binds[4].is_null = &quality_is_null;
  binds[5].buffer_type = MYSQL_TYPE_STRING;
  binds[5].buffer = tradable_buf;
  binds[5].buffer_length = sizeof(tradable_buf);
  binds[5].length = &tradable_len;
  binds[5].is_null = &tradable_is_null;
  binds[6].buffer_type = MYSQL_TYPE_STRING;
  binds[6].buffer = stattrak_buf;
  binds[6].buffer_length = sizeof(stattrak_buf);
  binds[6].length = &stattrak_len;
  binds[6].is_null = &stattrak_is_null;
  binds[7].buffer_type = MYSQL_TYPE_STRING;
  binds[7].buffer = stattrak_kills_buf;
  binds[7].buffer_length = sizeof(stattrak_kills_buf);
  binds[7].length = &stattrak_kills_len;
  binds[7].is_null = &stattrak_kills_is_null;
  binds[8].buffer_type = MYSQL_TYPE_STRING;
  binds[8].buffer = sticker_1_buf;
  binds[8].buffer_length = sizeof(sticker_1_buf);
  binds[8].length = &sticker_1_len;
  binds[8].is_null = &sticker_1_is_null;
  binds[9].buffer_type = MYSQL_TYPE_STRING;
  binds[9].buffer = sticker_1_wear_buf;
  binds[9].buffer_length = sizeof(sticker_1_wear_buf);
  binds[9].length = &sticker_1_wear_len;
  binds[9].is_null = &sticker_1_wear_is_null;
  binds[10].buffer_type = MYSQL_TYPE_STRING;
  binds[10].buffer = sticker_2_buf;
  binds[10].buffer_length = sizeof(sticker_2_buf);
  binds[10].length = &sticker_2_len;
  binds[10].is_null = &sticker_2_is_null;
  binds[11].buffer_type = MYSQL_TYPE_STRING;
  binds[11].buffer = sticker_2_wear_buf;
  binds[11].buffer_length = sizeof(sticker_2_wear_buf);
  binds[11].length = &sticker_2_wear_len;
  binds[11].is_null = &sticker_2_wear_is_null;
  binds[12].buffer_type = MYSQL_TYPE_STRING;
  binds[12].buffer = sticker_3_buf;
  binds[12].buffer_length = sizeof(sticker_3_buf);
  binds[12].length = &sticker_3_len;
  binds[12].is_null = &sticker_3_is_null;
  binds[13].buffer_type = MYSQL_TYPE_STRING;
  binds[13].buffer = sticker_3_wear_buf;
  binds[13].buffer_length = sizeof(sticker_3_wear_buf);
  binds[13].length = &sticker_3_wear_len;
  binds[13].is_null = &sticker_3_wear_is_null;
  binds[14].buffer_type = MYSQL_TYPE_STRING;
  binds[14].buffer = sticker_4_buf;
  binds[14].buffer_length = sizeof(sticker_4_buf);
  binds[14].length = &sticker_4_len;
  binds[14].is_null = &sticker_4_is_null;
  binds[15].buffer_type = MYSQL_TYPE_STRING;
  binds[15].buffer = sticker_4_wear_buf;
  binds[15].buffer_length = sizeof(sticker_4_wear_buf);
  binds[15].length = &sticker_4_wear_len;
  binds[15].is_null = &sticker_4_wear_is_null;
  binds[16].buffer_type = MYSQL_TYPE_STRING;
  binds[16].buffer = sticker_5_buf;
  binds[16].buffer_length = sizeof(sticker_5_buf);
  binds[16].length = &sticker_5_len;
  binds[16].is_null = &sticker_5_is_null;
  binds[17].buffer_type = MYSQL_TYPE_STRING;
  binds[17].buffer = sticker_5_wear_buf;
  binds[17].buffer_length = sizeof(sticker_5_wear_buf);
  binds[17].length = &sticker_5_wear_len;
  binds[17].is_null = &sticker_5_wear_is_null;
  binds[18].buffer_type = MYSQL_TYPE_STRING;
  binds[18].buffer = nametag_buf;
  binds[18].buffer_length = sizeof(nametag_buf);
  binds[18].length = &nametag_len;
  binds[18].is_null = &nametag_is_null;
  binds[19].buffer_type = MYSQL_TYPE_STRING;
  binds[19].buffer = pattern_index_buf;
  binds[19].buffer_length = sizeof(pattern_index_buf);
  binds[19].length = &pattern_index_len;
  binds[19].is_null = &pattern_index_is_null;
  binds[20].buffer_type = MYSQL_TYPE_STRING;
  binds[20].buffer = equipped_ct_buf;
  binds[20].buffer_length = sizeof(equipped_ct_buf);
  binds[20].length = &equipped_ct_len;
  binds[20].is_null = &equipped_ct_is_null;
  binds[21].buffer_type = MYSQL_TYPE_STRING;
  binds[21].buffer = equipped_t_buf;
  binds[21].buffer_length = sizeof(equipped_t_buf);
  binds[21].length = &equipped_t_len;
  binds[21].is_null = &equipped_t_is_null;
  binds[22].buffer_type = MYSQL_TYPE_STRING;
  binds[22].buffer = acknowledged_buf;
  binds[22].buffer_length = sizeof(acknowledged_buf);
  binds[22].length = &acknowledged_len;
  binds[22].is_null = &acknowledged_is_null;
  binds[23].buffer_type = MYSQL_TYPE_STRING;
  binds[23].buffer = acquired_by_buf;
  binds[23].buffer_length = sizeof(acquired_by_buf);
  binds[23].length = &acquired_by_len;
  binds[23].is_null = &acquired_by_is_null;

  if (mysql_stmt_bind_result(stmtHandle, binds)) {
    logger::error("CheckAndSendNewItemsSince: Failed to bind result for "
                  "prepared statement: %s",
                  mysql_stmt_error(stmtHandle));
    mysql_stmt_free_result(stmtHandle);
    return false;
  }

  // find highest item id
  while (mysql_stmt_fetch(stmtHandle) == 0) {
    uint64_t itemId = id_is_null ? 0 : strtoull(id_buf, nullptr, 10);
    if (itemId > highestItemId) {
      highestItemId = itemId;
    }
  }
  mysql_stmt_data_seek(stmtHandle, 0); // Reset cursor to beginning

  // one item - SOSingleObject
  if (mysql_stmt_fetch(stmtHandle) == 0) {
    const char *row_data[24];
    row_data[0] = id_is_null ? nullptr : id_buf;
    row_data[1] = item_id_is_null ? nullptr : item_id_buf;
    row_data[2] = floatval_is_null ? nullptr : floatval_buf;
    row_data[3] = rarity_is_null ? nullptr : rarity_buf;
    row_data[4] = quality_is_null ? nullptr : quality_buf;
    row_data[5] = tradable_is_null ? nullptr : tradable_buf;
    row_data[6] = stattrak_is_null ? nullptr : stattrak_buf;
    row_data[7] = stattrak_kills_is_null ? nullptr : stattrak_kills_buf;
    row_data[8] = sticker_1_is_null ? nullptr : sticker_1_buf;
    row_data[9] = sticker_1_wear_is_null ? nullptr : sticker_1_wear_buf;
    row_data[10] = sticker_2_is_null ? nullptr : sticker_2_buf;
    row_data[11] = sticker_2_wear_is_null ? nullptr : sticker_2_wear_buf;
    row_data[12] = sticker_3_is_null ? nullptr : sticker_3_buf;
    row_data[13] = sticker_3_wear_is_null ? nullptr : sticker_3_wear_buf;
    row_data[14] = sticker_4_is_null ? nullptr : sticker_4_buf;
    row_data[15] = sticker_4_wear_is_null ? nullptr : sticker_4_wear_buf;
    row_data[16] = sticker_5_is_null ? nullptr : sticker_5_buf;
    row_data[17] = sticker_5_wear_is_null ? nullptr : sticker_5_wear_buf;
    row_data[18] = nametag_is_null ? nullptr : nametag_buf;
    row_data[19] = pattern_index_is_null ? nullptr : pattern_index_buf;
    row_data[20] = equipped_ct_is_null ? nullptr : equipped_ct_buf;
    row_data[21] = equipped_t_is_null ? nullptr : equipped_t_buf;
    row_data[22] = acknowledged_is_null ? nullptr : acknowledged_buf;
    row_data[23] = acquired_by_is_null ? nullptr : acquired_by_buf;

    auto item = CreateItemFromDatabaseRow(steamId, (MYSQL_ROW)row_data);
    if (item) {
      bool isFromCrate = (acquired_by_buf && strcmp(acquired_by_buf, "0") == 0);
      bool isCrafted = (acquired_by_buf && strcmp(acquired_by_buf, "8") == 0);

      if (isFromCrate || isCrafted) {
        // Item from crate or craft - skip sending it here since it was already
        // sent in HandleUnboxCrate or HandleCraft response
        logger::info("CheckAndSendNewItemsSince: Skipping item %llu with "
                     "acquired_by='%s' (already sent in specific response)",
                     item->id(), acquired_by_buf ? acquired_by_buf : "null");

        // For gift types, we need to update acquired_by - SQL injection safe
        auto updateStmtOpt = createPreparedStatement(
            inventory_db,
            "UPDATE csgo_items SET acquired_by = 'crate' WHERE id = ?");

        if (updateStmtOpt) {
          auto &uStmt = *updateStmtOpt;
          uint64_t idParam = item->id();
          uStmt.bindUint64(0, &idParam);
          if (!uStmt.execute()) {
            logger::error("CheckAndSendNewItemsSince: Failed to update "
                          "acquired_by field: %s",
                          uStmt.error());
          }
        }

        updateSuccess =
            true; // Mark as success even though we didn't send anything
      } else {
        // For other items, send as standard SOSingleObject
        logger::info("CheckAndSendNewItemsSince: Sending 1 new item with "
                     "SOSingleObject");
        updateSuccess =
            SendSOSingleObject(p2psocket, steamId, SOTypeItem, *item);
      }
    }
  }

  if (highestItemId > lastItemId) {
    if (updateSuccess) {
      logger::info("CheckAndSendNewItemsSince: Successfully sent new items to "
                   "player %llu",
                   steamId);
    } else {
      logger::warning("CheckAndSendNewItemsSince: Failed to send new items to "
                      "player %llu, updating lastItemId anyway",
                      steamId);
    }

    logger::info(
        "CheckAndSendNewItemsSince: Updated lastItemId from %llu to %llu",
        lastItemId, highestItemId);
    lastItemId = highestItemId;
  }

  return updateSuccess;
}

// helper for new item notif
uint64_t GCNetwork_Inventory::GetLatestItemIdForUser(uint64_t steamId,
                                                     MYSQL *inventory_db) {
  if (!inventory_db) {
    logger::error("GetLatestItemIdForUser: Database connection is null");
    return 0;
  }

  // SQL injection safe: using prepared statement
  std::string steamId2 = GCNetwork_Users::SteamID64ToSteamID2(steamId);
  auto stmtOpt = createPreparedStatement(
      inventory_db, "SELECT MAX(id) FROM csgo_items WHERE owner_steamid2 = ?");

  if (!stmtOpt) {
    logger::error("GetLatestItemIdForUser: Failed to prepare statement");
    return 0;
  }

  auto &stmt = *stmtOpt;
  unsigned long steamIdLen = steamId2.length();
  stmt.bindString(0, steamId2.c_str(), &steamIdLen);

  if (!stmt.execute() || !stmt.storeResult()) {
    logger::error("GetLatestItemIdForUser: MySQL query failed: %s",
                  stmt.error());
    return 0;
  }

  uint64_t maxId = 0;
  MYSQL_BIND result[1];
  memset(result, 0, sizeof(result));

  result[0].buffer_type = MYSQL_TYPE_LONGLONG;
  result[0].buffer = &maxId;
  result[0].is_unsigned = 1;

  if (!stmt.bindResult(result)) {
    return 0;
  }

  if (stmt.fetch() == 0) {
    // maxId is already updated by fetch
  }

  logger::info(
      "GetLatestItemIdForUser: Found highest item ID %llu for user %llu", maxId,
      steamId);
  return maxId;
}

/**
 * Process an item acknowledgment message from the client
 * Handles multiple items being acknowledged at once
 *
 * @param p2psocket Socket for the client
 * @param steamId The steam ID of the player
 * @param message The acknowledgment message from the client
 * @param inventory_db Database connection
 * @return The number of items successfully acknowledged
 */
int GCNetwork_Inventory::ProcessClientAcknowledgment(
    SNetSocket_t p2psocket, uint64_t steamId,
    const CMsgGC_CC_CL2GC_ItemAcknowledged &message, MYSQL *inventory_db) {
  if (!inventory_db) {
    logger::error("ProcessClientAcknowledgment: Database connection is null");
    return 0;
  }

  if (message.item_id_size() == 0) {
    logger::warning(
        "ProcessClientAcknowledgment: Empty acknowledgment message received");
    return 0;
  }

  logger::info("ProcessClientAcknowledgment: Processing acknowledgment for %d "
               "items from player %llu",
               message.item_id_size(), steamId);

  // get the current highest inventory position for this user - SQL injection
  // safe
  std::string steamId2 = GCNetwork_Users::SteamID64ToSteamID2(steamId);
  auto maxStmtOpt = createPreparedStatement(
      inventory_db,
      "SELECT COALESCE(MAX(acknowledged), 1) FROM csgo_items WHERE "
      "owner_steamid2 = ? AND acknowledged < 1073741824");

  if (!maxStmtOpt) {
    logger::error("ProcessClientAcknowledgment: Failed to prepare max position "
                  "statement");
    return 0;
  }

  auto &maxStmt = *maxStmtOpt;
  unsigned long steamIdLen = steamId2.length();
  maxStmt.bindString(0, steamId2.c_str(), &steamIdLen);

  if (!maxStmt.execute() || !maxStmt.storeResult()) {
    logger::error("ProcessClientAcknowledgment: Failed to get max position: %s",
                  maxStmt.error());
    return 0;
  }

  uint32_t current_max_pos = 1;
  MYSQL_BIND maxBind[1];
  memset(maxBind, 0, sizeof(maxBind));
  maxBind[0].buffer_type = MYSQL_TYPE_LONG;
  maxBind[0].buffer = &current_max_pos;

  if (!maxStmt.bindResult(maxBind) || maxStmt.fetch() != 0) {
    // defaults to 1
  }

  uint32_t next_position = current_max_pos;

  // start transaction
  if (mysql_query(inventory_db, "START TRANSACTION") != 0) {
    logger::error(
        "ProcessClientAcknowledgment: Failed to start transaction: %s",
        mysql_error(inventory_db));
    return 0;
  }

  int successCount = 0;

  // one item?
  bool isSingleItem = (message.item_id_size() == 1);
  std::unique_ptr<CSOEconItem> singleItem = nullptr;

  // For multiple items, prepare the multiple objects message
  CMsgSOMultipleObjects updateMsg;
  if (!isSingleItem) {
    InitMultipleObjectsMessage(updateMsg, steamId);
  }

  // Prepare the update statement once outside the loop for security and
  // performance
  auto stmtOpt = createPreparedStatement(
      inventory_db, "UPDATE csgo_items SET acknowledged = ? "
                    "WHERE id = ? AND owner_steamid2 = ? AND (acknowledged = 0 "
                    "OR acknowledged IS NULL OR acknowledged >= 1073741824)");

  if (!stmtOpt) {
    logger::error("ProcessClientAcknowledgment: Failed to prepare statement");
    mysql_query(inventory_db, "ROLLBACK");
    return 0;
  }

  auto &stmt = *stmtOpt;

  // loop over all items to acknowledge
  for (int i = 0; i < message.item_id_size(); i++) {
    uint64_t itemId = message.item_id(i);

    next_position++;
    if (next_position == 1)
      next_position = 2; // skip position 1 (cause thats the nametag)

    // SQL injection safe: using prepared statement
    uint32_t posParam = next_position;
    uint64_t idParam = itemId;
    stmt.bindUint32(0, &posParam);
    stmt.bindUint64(1, &idParam);
    stmt.bindString(2, steamId2.c_str(), &steamIdLen);

    if (!stmt.execute()) {
      logger::error(
          "ProcessClientAcknowledgment: MySQL query failed for item %llu: %s",
          itemId, stmt.error());
      continue;
    }

    if (mysql_affected_rows(inventory_db) == 0) {
      logger::warning("ProcessClientAcknowledgment: Item %llu not found or "
                      "already acknowledged",
                      itemId);
      next_position--;
      continue;
    }

    // acknowledge success
    successCount++;

    // make item for notification
    auto item =
        FetchItemFromDatabase(itemId, steamId, inventory_db, next_position);
    if (item) {
      if (isSingleItem) {
        singleItem = std::move(item);
      } else {
        AddToMultipleObjectsMessage(updateMsg, SOTypeItem, *item);
      }
    }
  }

  // process result
  if (successCount > 0) {
    if (mysql_query(inventory_db, "COMMIT") != 0) {
      logger::error(
          "ProcessClientAcknowledgment: Failed to commit transaction: %s",
          mysql_error(inventory_db));
      mysql_query(inventory_db, "ROLLBACK");
      return 0;
    }

    if (isSingleItem && singleItem) {
      // Send single item update
      logger::info("ProcessClientAcknowledgment: Sending single item update "
                   "with SOSingleObject for item %llu",
                   singleItem->id());
      SendSOSingleObject(p2psocket, steamId, SOTypeItem, *singleItem);
    } else if (!isSingleItem && updateMsg.objects_modified_size() > 0) {
      // Send multiple items update
      logger::info("ProcessClientAcknowledgment: Sending %d modified items "
                   "with SOMultipleObjects",
                   updateMsg.objects_modified_size());
      SendSOMultipleObjects(p2psocket, updateMsg);
    }

    logger::info("ProcessClientAcknowledgment: Successfully acknowledged %d "
                 "items for player %llu",
                 successCount, steamId);
  } else {
    mysql_query(inventory_db, "ROLLBACK");
    logger::warning("ProcessClientAcknowledgment: No items were acknowledged, "
                    "transaction rolled back");
  }

  return successCount;
}

/**
 * Gets the next available inventory position for a new item
 *
 * @param steamId The steam ID of the player
 * @param inventory_db Database connection
 * @return The next available inventory position (skips position 1 for
 * nametag)
 */
uint32_t GCNetwork_Inventory::GetNextInventoryPosition(uint64_t steamId,
                                                       MYSQL *inventory_db) {
  if (!inventory_db) {
    logger::error("GetNextInventoryPosition: Database connection is null");
    return 2; // Default to position 2 if we can't query
  }

  // SQL injection safe: using prepared statement
  std::string steamId2 = GCNetwork_Users::SteamID64ToSteamID2(steamId);
  auto stmtOpt = createPreparedStatement(
      inventory_db,
      "SELECT COALESCE(MAX(acknowledged), 1) FROM csgo_items WHERE "
      "owner_steamid2 = ?");

  if (!stmtOpt) {
    logger::error("GetNextInventoryPosition: Failed to prepare statement");
    return 2;
  }

  auto &stmt = *stmtOpt;
  unsigned long steamIdLen = steamId2.length();
  stmt.bindString(0, steamId2.c_str(), &steamIdLen);

  if (!stmt.execute() || !stmt.storeResult()) {
    logger::error("GetNextInventoryPosition: MySQL query failed: %s",
                  stmt.error());
    return 2;
  }

  uint32_t currentMaxPos = 1;
  MYSQL_BIND resultBind[1];
  memset(resultBind, 0, sizeof(resultBind));

  resultBind[0].buffer_type = MYSQL_TYPE_LONG;
  resultBind[0].buffer = &currentMaxPos;

  if (!stmt.bindResult(resultBind) || stmt.fetch() != 0) {
    // If fetch fails, we'll use the default currentMaxPos = 1
  }

  uint32_t nextPosition = currentMaxPos + 1;
  if (nextPosition <= 1)
    nextPosition = 2; // Skip position 1 (reserved for nametag)

  logger::info(
      "GetNextInventoryPosition: Next available position for user %llu is %u",
      steamId, nextPosition);

  return nextPosition;
}

/**
 * Handles the unboxing of a crate, generating a new item and saving it to the
 * database
 *
 * @param p2psocket The socket to send updates to
 * Saves a newly generated item to the database
 *
 * @param item The CSOEconItem to save
 * @param steamId The steam ID of the owner
 * @param inventory_db Database connection
 * @param isBaseWeapon Whether this is a base weapon without a skin (default:
 * false)
 * @return The ID of the newly created item, or 0 on failure
 */
uint64_t GCNetwork_Inventory::SaveNewItemToDatabase(const CSOEconItem &item,
                                                    uint64_t steamId,
                                                    MYSQL *inventory_db,
                                                    bool isBaseWeapon) {
  if (!g_itemSchema || !inventory_db) {
    logger::error(
        "SaveNewItemToDatabase: ItemSchema or database connection is null");
    return 0;
  }

  // extract item info
  uint32_t defIndex = item.def_index();
  uint32_t quality = item.quality();

  // for base weapons use NULL for
  bool isBaseItem = isBaseWeapon;
  uint32_t rarity =
      isBaseItem ? 0 : (item.rarity() > 0 ? item.rarity() - 1 : 0);

  // default, some will get populated later
  std::string itemIdStr = "";
  float floatValue = 0.0f;
  uint32_t paintIndex = 0;
  uint32_t patternIndex = 0;
  bool statTrak = false;
  uint32_t statTrakKills = 0;
  std::string nameTag = item.has_custom_name() ? item.custom_name() : "";
  bool tradable = isBaseItem ? false : true; // Set to 0 for base items
  std::string acquiredBy =
      isBaseItem ? "default" : "0"; // Set to "default" for base items
  if (item.has_origin()) {
    acquiredBy = std::to_string(item.origin());
  }

  std::string itemName = "";
  std::string weaponType = "";
  std::string weaponId = "";
  std::string weaponSlot = "0";
  std::string wearName = "Factory New";

  // attributes
  // Modify the SaveNewItemToDatabase function to correctly handle souvenir
  // items Only showing the relevant part that needs to be changed

  // attributes
  for (int i = 0; i < item.attribute_size(); i++) {
    const CSOEconItemAttribute &attr = item.attribute(i);
    uint32_t attrDefIndex = attr.def_index();

    switch (attrDefIndex) {
    case ATTR_PAINT_INDEX: // paint kit - this is the texture prefab
      paintIndex = g_itemSchema->AttributeUint32(&attr);
      break;

    case ATTR_PAINT_WEAR: // float
      floatValue = g_itemSchema->AttributeFloat(&attr);
      // wear name
      if (floatValue < 0.07f)
        wearName = "Factory New";
      else if (floatValue < 0.15f)
        wearName = "Minimal Wear";
      else if (floatValue < 0.38f)
        wearName = "Field-Tested";
      else if (floatValue < 0.45f)
        wearName = "Well-Worn";
      else
        wearName = "Battle-Scarred";
      break;

    case ATTR_PAINT_SEED: // seed
      patternIndex = g_itemSchema->AttributeUint32(&attr);
      break;

    case ATTR_KILLEATER_SCORE: // StatTrak
      statTrak = true;
      statTrakKills = g_itemSchema->AttributeUint32(&attr);
      break;

    case ATTR_ITEM_STICKER_ID:
      if (defIndex == 1209) {
        paintIndex = g_itemSchema->AttributeUint32(&attr);
      }
      break;

    case ATTR_ITEM_MUSICKIT_ID:
      if (defIndex == 1314) {
        paintIndex = g_itemSchema->AttributeUint32(&attr);
      }
      break;
    }
  }

  // get attributes for stickers only
  std::vector<std::pair<uint32_t, float>> stickers;
  stickers.resize(5, {0, 0.0f}); // 5 max

  for (int i = 0; i < item.attribute_size(); i++) {
    const CSOEconItemAttribute &attr = item.attribute(i);
    uint32_t attrDefIndex = attr.def_index();

    // sticker attributes (113, 117, 121, 125, 129)
    if (attrDefIndex >= 113 && attrDefIndex <= 133 &&
        (attrDefIndex - 113) % 4 == 0) {
      uint32_t stickerPos = (attrDefIndex - 113) / 4;
      if (stickerPos < stickers.size()) {
        uint32_t stickerId = g_itemSchema->AttributeUint32(&attr);

        float stickerWear = 0.0f;
        for (int j = 0; j < item.attribute_size(); j++) {
          const CSOEconItemAttribute &wearAttr = item.attribute(j);
          if (wearAttr.def_index() == attrDefIndex + 1) {
            stickerWear = g_itemSchema->AttributeFloat(&wearAttr);
            break;
          }
        }

        stickers[stickerPos] = {stickerId, stickerWear};
      }
    }
  }

  // get info
  if (!GetWeaponInfo(defIndex, weaponType, weaponId)) {
    // If GetWeaponInfo fails, try to get info from the ItemSchema
    auto itemInfoIter = g_itemSchema->m_itemInfo.find(defIndex);
    if (itemInfoIter != g_itemSchema->m_itemInfo.end()) {
      const auto &itemInfo = itemInfoIter->second;
      std::string_view displayName = itemInfo.GetDisplayName();
      weaponType = std::string(displayName);
      weaponId = "weapon_" + std::string(itemInfo.m_name);
    } else {
      // If all else fails, use generic naming
      weaponType = "Unknown Weapon";
      weaponId = "weapon_" + std::to_string(defIndex);
    }
  }

  // Set item name based on weapon type and paint index
  itemName = weaponType;

  // Construct item_id string based on item type
  if (defIndex == 1209) // Sticker
  {
    itemIdStr = "sticker-" + std::to_string(paintIndex);
  } else if (defIndex == 1314) // Music kit
  {
    itemIdStr = "music_kit-" + std::to_string(paintIndex);
  } else if (defIndex >= 500 && defIndex <= 552) // Knife
  {
    if (isBaseWeapon || paintIndex == 0) {
      // Base knife with no skin
      itemIdStr = "skin-" + std::to_string(defIndex) + "_0_0";
    } else {
      // Knife with skin
      itemIdStr = "skin-" + std::to_string(defIndex) + "_" +
                  std::to_string(paintIndex) + "_0";
    }
  } else // Regular weapon
  {
    if (isBaseWeapon || paintIndex == 0) {
      // Base weapon with no skin
      itemIdStr = "skin-" + std::to_string(defIndex) + "_0_0";
    } else {
      // Weapon with skin
      itemIdStr = "skin-" + std::to_string(defIndex) + "_" +
                  std::to_string(paintIndex) + "_0";
    }
  }

  // For weapons with a paint kit (skin), add the skin name to the item name
  if (paintIndex > 0 && defIndex != 1209 && defIndex != 1314 && !isBaseWeapon) {
    // Look for the paint kit info
    for (const auto &[name, paintKit] : g_itemSchema->m_paintKitInfo) {
      if (paintKit.m_defIndex == paintIndex) {
        // Get the skin name
        std::string_view skinName = paintKit.GetDisplayName();
        if (!skinName.empty()) {
          itemName = itemName + " | " + std::string(skinName);
          break;
        }
      }
    }
  }

  // Calculate sticker slots based on weapon type (most weapons have 4-5
  // slots)
  uint32_t stickerSlots = 0;
  if (defIndex != 1209 && defIndex != 1314) {
    // Special cases for weapons with 5 sticker slots
    if (defIndex == 11 || defIndex == 64) { // G3SG1 or R8 Revolver
      stickerSlots = 5;
    } else {
      // Default for most weapons: 4 sticker slots
      stickerSlots = 4;
    }
  }

  // SQL injection safe: using prepared statement for the entire massive
  // INSERT We use a fixed column list to enable statement preparation and
  // security.
  auto stmtOpt = createPreparedStatement(
      inventory_db,
      "INSERT INTO csgo_items ("
      "owner_steamid2, item_id, name, nametag, weapon_type, weapon_id, "
      "weapon_slot, wear, floatval, paint_index, pattern_index, rarity, "
      "quality, tradable, commodity, stattrak, stattrak_kills, "
      "sticker_slots, sticker_1, sticker_1_wear, sticker_2, sticker_2_wear, "
      "sticker_3, sticker_3_wear, sticker_4, sticker_4_wear, "
      "sticker_5, sticker_5_wear, market_price, equipped_ct, "
      "equipped_t, acquired_by, acknowledged"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
      "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

  if (!stmtOpt) {
    logger::error("SaveNewItemToDatabase: Failed to prepare insert statement");
    return 0;
  }

  auto &stmt = *stmtOpt;
  std::string steamId2 = GCNetwork_Users::SteamID64ToSteamID2(steamId);
  unsigned long steamIdLen = steamId2.length();
  unsigned long itemIdLen = itemIdStr.length();
  unsigned long itemNameLen = itemName.length();
  unsigned long nameTagLen = nameTag.length();
  unsigned long weaponTypeLen = weaponType.length();
  unsigned long weaponIdLen = weaponId.length();
  unsigned long weaponSlotLen = weaponSlot.length();
  unsigned long wearNameLen = wearName.length();
  unsigned long acquiredByLen = acquiredBy.length();

  // Bind parameters
  stmt.bindString(0, steamId2.c_str(), &steamIdLen);
  stmt.bindString(1, itemIdStr.c_str(), &itemIdLen);
  stmt.bindString(2, itemName.c_str(), &itemNameLen);

  if (!nameTag.empty()) {
    stmt.bindString(3, nameTag.c_str(), &nameTagLen);
  } else {
    stmt.bindNull(3);
  }

  stmt.bindString(4, weaponType.c_str(), &weaponTypeLen);
  stmt.bindString(5, weaponId.c_str(), &weaponIdLen);
  stmt.bindString(6, weaponSlot.c_str(), &weaponSlotLen);
  stmt.bindString(7, wearName.c_str(), &wearNameLen);

  if (isBaseItem) {
    stmt.bindNull(8);
  } else {
    stmt.bindFloat(8, &floatValue);
  }

  stmt.bindUint32(9, &paintIndex);
  stmt.bindUint32(10, &patternIndex);

  if (isBaseItem) {
    stmt.bindNull(11);
  } else {
    stmt.bindUint32(11, &rarity);
  }

  stmt.bindUint32(12, &quality);
  uint32_t tradableVal = tradable ? 1 : 0;
  stmt.bindUint32(13, &tradableVal);
  uint32_t commodityVal = 0;
  stmt.bindUint32(14, &commodityVal);
  uint32_t statTrakVal = statTrak ? 1 : 0;
  stmt.bindUint32(15, &statTrakVal);

  if (statTrak) {
    stmt.bindUint32(16, &statTrakKills);
  } else {
    stmt.bindNull(16);
  }

  stmt.bindUint32(17, &stickerSlots);

  // Bind stickers (18-27)
  for (int i = 0; i < 5; i++) {
    if (i < static_cast<int>(stickers.size()) && stickers[i].first > 0) {
      stmt.bindUint32(18 + (i * 2), &stickers[i].first);
      stmt.bindFloat(19 + (i * 2), &stickers[i].second);
    } else {
      stmt.bindNull(18 + (i * 2));
      stmt.bindNull(19 + (i * 2));
    }
  }

  float marketPrice = 0.0f;
  stmt.bindFloat(28, &marketPrice);
  uint32_t zeroVal = 0;
  stmt.bindUint32(29, &zeroVal); // equipped_ct
  stmt.bindUint32(30, &zeroVal); // equipped_t
  stmt.bindString(31, acquiredBy.c_str(), &acquiredByLen);
  uint32_t inventoryVal = item.inventory();
  stmt.bindUint32(32, &inventoryVal); // acknowledged/inventory

  if (!stmt.execute()) {
    logger::error("SaveNewItemToDatabase: MySQL query failed: %s",
                  stmt.error());
    return 0;
  }

  // Get the newly inserted item ID
  uint64_t newItemId = mysql_insert_id(inventory_db);
  logger::info(
      "SaveNewItemToDatabase: Successfully inserted new item with ID %llu",
      newItemId);

  return newItemId;
}

/**
 * Gets the display name and weapon identifier for a given defIndex
 *
 * @param defIndex The definition index of the weapon
 * @param weaponName Output parameter for the weapon's display name
 * @param weaponId Output parameter for the weapon's identifier
 * @return True if the information was found, false otherwise
 */
bool GCNetwork_Inventory::GetWeaponInfo(uint32_t defIndex,
                                        std::string &weaponName,
                                        std::string &weaponId) {
  weaponName = "";
  weaponId = "";

  // Check if this is a known weapon type
  switch (defIndex) {
  // Pistols
  case 1:
    weaponName = "Desert Eagle";
    weaponId = "weapon_deagle";
    break;
  case 2:
    weaponName = "Dual Berettas";
    weaponId = "weapon_elite";
    break;
  case 3:
    weaponName = "Five-SeveN";
    weaponId = "weapon_fiveseven";
    break;
  case 4:
    weaponName = "Glock-18";
    weaponId = "weapon_glock";
    break;
  case 30:
    weaponName = "Tec-9";
    weaponId = "weapon_tec9";
    break;
  case 32:
    weaponName = "P2000";
    weaponId = "weapon_hkp2000";
    break;
  case 36:
    weaponName = "P250";
    weaponId = "weapon_p250";
    break;
  case 61:
    weaponName = "USP-S";
    weaponId = "weapon_usp_silencer";
    break;
  case 63:
    weaponName = "CZ75-Auto";
    weaponId = "weapon_cz75a";
    break;
  case 64:
    weaponName = "R8 Revolver";
    weaponId = "weapon_revolver";
    break;

  // Rifles
  case 7:
    weaponName = "AK-47";
    weaponId = "weapon_ak47";
    break;
  case 8:
    weaponName = "AUG";
    weaponId = "weapon_aug";
    break;
  case 9:
    weaponName = "AWP";
    weaponId = "weapon_awp";
    break;
  case 10:
    weaponName = "FAMAS";
    weaponId = "weapon_famas";
    break;
  case 11:
    weaponName = "G3SG1";
    weaponId = "weapon_g3sg1";
    break;
  case 13:
    weaponName = "Galil AR";
    weaponId = "weapon_galilar";
    break;
  case 16:
    weaponName = "M4A4";
    weaponId = "weapon_m4a1";
    break;
  case 38:
    weaponName = "SCAR-20";
    weaponId = "weapon_scar20";
    break;
  case 39:
    weaponName = "SG 553";
    weaponId = "weapon_sg556";
    break;
  case 40:
    weaponName = "SSG 08";
    weaponId = "weapon_ssg08";
    break;
  case 60:
    weaponName = "M4A1-S";
    weaponId = "weapon_m4a1_silencer";
    break;

  // SMGs
  case 17:
    weaponName = "MAC-10";
    weaponId = "weapon_mac10";
    break;
  case 19:
    weaponName = "P90";
    weaponId = "weapon_p90";
    break;
  case 23:
    weaponName = "MP5-SD";
    weaponId = "weapon_mp5sd";
    break;
  case 24:
    weaponName = "UMP-45";
    weaponId = "weapon_ump45";
    break;
  case 26:
    weaponName = "PP-Bizon";
    weaponId = "weapon_bizon";
    break;
  case 33:
    weaponName = "MP7";
    weaponId = "weapon_mp7";
    break;
  case 34:
    weaponName = "MP9";
    weaponId = "weapon_mp9";
    break;

  // Heavy
  case 14:
    weaponName = "M249";
    weaponId = "weapon_m249";
    break;
  case 25:
    weaponName = "XM1014";
    weaponId = "weapon_xm1014";
    break;
  case 27:
    weaponName = "MAG-7";
    weaponId = "weapon_mag7";
    break;
  case 28:
    weaponName = "Negev";
    weaponId = "weapon_negev";
    break;
  case 29:
    weaponName = "Sawed-Off";
    weaponId = "weapon_sawedoff";
    break;
  case 35:
    weaponName = "Nova";
    weaponId = "weapon_nova";
    break;

  // Default Knives
  case 42:
    weaponName = "Knife (CT)";
    weaponId = "weapon_knife";
    break;
  case 59:
    weaponName = "Knife (T)";
    weaponId = "weapon_knife_t";
    break;

  // Special Knives
  case 500:
    weaponName = "Bayonet";
    weaponId = "weapon_bayonet";
    break;
  case 503:
    weaponName = "Classic Knife";
    weaponId = "weapon_knife_css";
    break;
  case 505:
    weaponName = "Flip Knife";
    weaponId = "weapon_knife_flip";
    break;
  case 506:
    weaponName = "Gut Knife";
    weaponId = "weapon_knife_gut";
    break;
  case 507:
    weaponName = "Karambit";
    weaponId = "weapon_knife_karambit";
    break;
  case 508:
    weaponName = "M9 Bayonet";
    weaponId = "weapon_knife_m9_bayonet";
    break;
  case 509:
    weaponName = "Huntsman Knife";
    weaponId = "weapon_knife_tactical";
    break;
  case 512:
    weaponName = "Falchion Knife";
    weaponId = "weapon_knife_falchion";
    break;
  case 514:
    weaponName = "Bowie Knife";
    weaponId = "weapon_knife_survival_bowie";
    break;
  case 515:
    weaponName = "Butterfly Knife";
    weaponId = "weapon_knife_butterfly";
    break;
  case 516:
    weaponName = "Shadow Daggers";
    weaponId = "weapon_knife_push";
    break;
  case 517:
    weaponName = "Paracord Knife";
    weaponId = "weapon_knife_cord";
    break;
  case 518:
    weaponName = "Survival Knife";
    weaponId = "weapon_knife_canis";
    break;
  case 519:
    weaponName = "Ursus Knife";
    weaponId = "weapon_knife_ursus";
    break;
  case 520:
    weaponName = "Navaja Knife";
    weaponId = "weapon_knife_gypsy_jackknife";
    break;
  case 521:
    weaponName = "Nomad Knife";
    weaponId = "weapon_knife_outdoor";
    break;
  case 522:
    weaponName = "Stiletto Knife";
    weaponId = "weapon_knife_stiletto";
    break;
  case 523:
    weaponName = "Talon Knife";
    weaponId = "weapon_knife_widowmaker";
    break;
  case 525:
    weaponName = "Skeleton Knife";
    weaponId = "weapon_knife_skeleton";
    break;

  // Equipment
  case 31:
    weaponName = "Zeus x27";
    weaponId = "weapon_taser";
    break;
  case 49:
    weaponName = "C4";
    weaponId = "weapon_c4";
    break;

  // Other special items
  case 1209:
    weaponName = "Sticker";
    weaponId = "sticker";
    break;
  case 1314:
    weaponName = "Music Kit";
    weaponId = "music_kit";
    break;

  default:
    // Try to fetch from ItemSchema for unknown defIndex
    if (g_itemSchema) {
      auto itemInfoIter = g_itemSchema->m_itemInfo.find(defIndex);
      if (itemInfoIter != g_itemSchema->m_itemInfo.end()) {
        const auto &itemInfo = itemInfoIter->second;
        std::string_view displayName = itemInfo.GetDisplayName();
        weaponName = std::string(displayName);
        weaponId = "weapon_" + std::string(itemInfo.m_name);
        return !weaponName.empty();
      }
    }
    return false;
  }

  return true;
}

//
// ITEM UPDATES
//

/**
 * Deletes an item from the database and notifies the client
 *
 * @param p2psocket The socket to notify (optional)
 * @param steamId The steam ID of the item's owner
 * @param itemId The unique ID of the item to delete
 * @param inventory_db Database connection to delete the item from
 * @return True if the item was successfully deleted
 */
bool GCNetwork_Inventory::DeleteItem(SNetSocket_t p2psocket, uint64_t steamId,
                                     uint64_t itemId, MYSQL *inventory_db) {
  if (!inventory_db) {
    logger::error("DeleteItem: Database connection is null");
    return false;
  }

  // Fetch the item before deleting it, so we can send its information
  auto item = FetchItemFromDatabase(itemId, steamId, inventory_db);
  if (!item) {
    logger::error(
        "DeleteItem: Item %llu not found or doesn't belong to user %llu",
        itemId, steamId);
    return false;
  }

  // Delete from database - SQL injection safe using prepared statement
  std::string steamId2 = GCNetwork_Users::SteamID64ToSteamID2(steamId);

  auto stmtOpt = createPreparedStatement(
      inventory_db,
      "DELETE FROM csgo_items WHERE id = ? AND owner_steamid2 = ?");

  if (!stmtOpt) {
    logger::error("DeleteItem: Failed to prepare delete statement");
    return false;
  }

  auto &stmt = *stmtOpt;
  uint64_t itemIdParam = itemId;
  unsigned long steamIdLen = steamId2.length();
  stmt.bindUint64(0, &itemIdParam);
  stmt.bindString(1, steamId2.c_str(), &steamIdLen);

  if (!stmt.execute()) {
    logger::error("DeleteItem: MySQL delete query failed: %s", stmt.error());
    return false;
  }

  if (stmt.affectedRows() == 0) {
    logger::warning("DeleteItem: No rows affected when deleting item %llu",
                    itemId);
    return false;
  }

  logger::info("DeleteItem: Successfully deleted item %llu from database",
               itemId);

  if (p2psocket != 0) {
    logger::info("DeleteItem: Sending delete notification for item %llu to "
                 "player %llu",
                 itemId, steamId);

    bool success = SendSOSingleObject(p2psocket, steamId, SOTypeItem, *item,
                                      k_EMsgGC_CC_DeleteItem);
    if (!success) {
      logger::error("DeleteItem: Failed to send delete notification to client");
      return false;
    }
  }

  return true;
}

/**
 * Sends a single object update to the client
 *
 * @param p2psocket The socket to send the message on
 * @param steamId The steam ID of the player to update
 * @param type_id The type of object being updated (e.g. SOTypeItem)
 * @param object The protobuf object to send
 * @param messageType The message type to use (defaults to
 * k_EMsgGC_CC_GC2CL_SOSingleObject)
 * @return True if message was sent successfully
 */
bool GCNetwork_Inventory::SendSOSingleObject(
    SNetSocket_t p2psocket, uint64_t steamId, SOTypeId type,
    const google::protobuf::MessageLite &object, uint32_t messageType) {
  CMsgSOSingleObject message;

  // Set the type ID
  message.set_type_id(type);

  // Serialize the object to binary and set as object_data
  message.set_object_data(object.SerializeAsString());

  // Set the version
  message.set_version(InventoryVersion);

  // Set the owner ID
  auto *owner = message.mutable_owner_soid();
  owner->set_type(SoIdTypeSteamId);
  owner->set_id(steamId);

  // Create a network message and send it
  NetworkMessage responseMsg = NetworkMessage::FromProto(message, messageType);

  logger::info("SendSOSingleObject: Sending object of type %d to %llu with "
               "message type %u, size: %u bytes",
               type, steamId, messageType, responseMsg.GetTotalSize());

  bool success = responseMsg.WriteToSocket(p2psocket, true);
  if (!success) {
    logger::error("SendSOSingleObject: Failed to write message to socket - "
                  "client likely disconnected");
  }

  return success;
}

/**
 * Helper to add an object to a multiple objects message
 *
 * @param message The multiple objects message to add to
 * @param type The type of object to add
 * @param object The object to serialize and add
 * @param collection Which collection to add to (added, modified, removed)
 */
void GCNetwork_Inventory::AddToMultipleObjectsMessage(
    CMsgSOMultipleObjects &message, SOTypeId type,
    const google::protobuf::MessageLite &object,
    const std::string &collection) {
  CMsgSOMultipleObjects::SingleObject *single;

  if (collection == "added") {
    single = message.add_objects_added();
  } else if (collection == "removed") {
    single = message.add_objects_removed();
  } else {
    // Default to "modified"
    single = message.add_objects_modified();
  }

  single->set_type_id(type);
  single->set_object_data(object.SerializeAsString());
}

/**
 * Initialize a multiple objects message with owner ID and version
 *
 * @param message The message to initialize
 * @param steamId The steam ID of the owner
 */
void GCNetwork_Inventory::InitMultipleObjectsMessage(
    CMsgSOMultipleObjects &message, uint64_t steamId) {
  message.set_version(InventoryVersion);
  auto *owner = message.mutable_owner_soid();
  owner->set_type(SoIdTypeSteamId);
  owner->set_id(steamId);
}

/**
 * Sends a multiple objects update to the client
 *
 * @param p2psocket The socket to send the message on
 * @param message The prepared multiple objects message
 * @return True if message was sent successfully
 */
bool GCNetwork_Inventory::SendSOMultipleObjects(
    SNetSocket_t p2psocket, const CMsgSOMultipleObjects &message) {
  // Create a network message and send it
  NetworkMessage responseMsg =
      NetworkMessage::FromProto(message, k_EMsgGC_CC_GC2CL_SOMultipleObjects);

  uint32_t totalModified = message.objects_modified_size();
  uint32_t totalAdded = message.objects_added_size();
  uint32_t totalRemoved = message.objects_removed_size();

  logger::info("SendSOMultipleObjects: Sending update with %u modified, %u "
               "added, %u removed objects",
               totalModified, totalAdded, totalRemoved);
  logger::info("SendSOMultipleObjects: Total message size: %u bytes",
               responseMsg.GetTotalSize());

  bool success = responseMsg.WriteToSocket(p2psocket, true);
  if (!success) {
    logger::error("SendSOMultipleObjects: Failed to write message to socket - "
                  "client likely disconnected");
  }

  return success;
}

// DEFAULT ITEM HANDLING

/**
 * Creates a base weapon item with default properties
 *
 * @param defIndex The definition index of the weapon to create
 * @param steamId The steam ID of the owner
 * @param inventory_db Database connection to save the item
 * @param saveToDb Whether to save the item to the database immediately
 * (default: true)
 * @param customName Optional custom name for the weapon
 * @return A pointer to the newly created CSOEconItem (caller is responsible
 * for freeing memory) or nullptr if creation failed
 */
std::unique_ptr<CSOEconItem>
GCNetwork_Inventory::CreateBaseItem(uint32_t defIndex, uint64_t steamId,
                                    MYSQL *inventory_db, bool saveToDb,
                                    const std::string &customName) {
  if (!g_itemSchema) {
    logger::error("CreateBaseItem: ItemSchema is null");
    return nullptr;
  }

  // Create the base item
  auto item = std::make_unique<CSOEconItem>();

  // Set basic properties
  item->set_account_id(steamId & 0xFFFFFFFF);
  item->set_def_index(defIndex);
  item->set_inventory(0);
  item->set_level(1);
  item->set_quantity(1);
  item->set_quality(ItemSchema::QualityNormal);
  item->set_flags(0);
  item->set_origin(kEconItemOrigin_Purchased);
  item->set_rarity(ItemSchema::RarityDefault);

  // Set custom name if provided
  if (!customName.empty()) {
    item->set_custom_name(customName);
  }

  // If saving to database is requested
  if (saveToDb && inventory_db != nullptr) {
    uint64_t newItemId = SaveNewItemToDatabase(
        *item, steamId, inventory_db, true); // Pass true for isBaseWeapon
    if (newItemId == 0) {
      logger::error("CreateBaseItem: Failed to save base item to database "
                    "(defIndex: %u)",
                    defIndex);
      return nullptr;
    }

    // Set the newly assigned ID
    item->set_id(newItemId);
    logger::info("CreateBaseItem: Created base item with defIndex %u, ID %llu "
                 "for player %llu",
                 defIndex, newItemId, steamId);
  } else {
    logger::info("CreateBaseItem: Created unsaved base item with defIndex %u "
                 "for player %llu",
                 defIndex, steamId);
  }

  return item;
}

bool GCNetwork_Inventory::IsDefaultItemId(uint64_t itemId, uint32_t &defIndex,
                                          uint32_t &paintKitIndex) {
  if ((itemId & ItemIdDefaultItemMask) == ItemIdDefaultItemMask) {
    defIndex = itemId & 0xffff;
    paintKitIndex = (itemId >> 16) & 0xffff;
    return true;
  }
  return false;
}

// EQUIPS

/**
 * Equips an item to a specific class (CT/T) and slot
 * (this function is so fucked)
 *
 * @param p2psocket The socket to send updates to
 * @param steamId The steam ID of the player
 * @param itemId The ID of the item to equip
 * @param classId The class ID to equip for (CLASS_CT or CLASS_T)
 * @param slotId The slot ID to equip to
 * @param inventory_db Database connection to update
 * @return True if the item was successfully equipped
 */

/**
 * Sends an equipment update to the client
 *
 * @param p2psocket The socket to send updates to
 * @param steamId The steam ID of the player
 * @param itemId The ID of the item being equipped
 * @param classId The class ID where the item is equipped
 * @param slotId The slot ID where the item is equipped
 * @param inventory_db Database connection
 * @return True if update was sent successfully
 */

/**
 * Sends an unequip update to the client
 *
 * @param p2psocket The socket to send updates to
 * @param steamId The steam ID of the player
 * @param itemId The ID of the item being unequipped
 * @param inventory_db Database connection
 * @param was_equipped_ct Whether the item was equipped for CT
 * @param was_equipped_t Whether the item was equipped for T
 * @param def_index The definition index of the item
 * @return True if update was sent successfully
 */

// RENAMING ITEMS

// STICKERS

/**
 * Handles scraping a sticker from an item
 * If wear reaches max, removes the sticker entirely
 *
 * @param p2psocket The socket to send updates to
 * @param steamId The steam ID of the player
 * @param message The sticker scrape message
 * @param inventory_db Database connection
 * @return True if sticker was successfully scraped
 */
