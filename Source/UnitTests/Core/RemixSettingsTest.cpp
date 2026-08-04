// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <variant>

#include "Core/Config/RemixSettings.h"

// The Remix settings tab is generated from GetRemixSettingsMetadata(), so a knob
// that is declared in RemixSettings.h but has no row there would simply not be
// editable, silently. The static_assert in RemixSettings.cpp catches a wrong row
// COUNT at compile time; these tests catch the ways the count can be right while
// the table is wrong - a duplicated row, a row pointing at a non-Remix setting,
// a combo with no entries.
namespace
{
const Config::Location& RowLocation(const Config::RemixSettingMeta& meta)
{
  return std::visit([](const auto* info) -> const Config::Location& { return info->GetLocation(); },
                    meta.setting);
}
}  // namespace

TEST(RemixSettings, MetadataCoversEverySetting)
{
  EXPECT_EQ(Config::GetRemixSettingsMetadata().size(), Config::kRemixSettingCount);
}

TEST(RemixSettings, EverySettingPointerIsValid)
{
  for (const Config::RemixSettingMeta& meta : Config::GetRemixSettingsMetadata())
  {
    const bool non_null = std::visit([](const auto* info) { return info != nullptr; }, meta.setting);
    EXPECT_TRUE(non_null);
  }
}

TEST(RemixSettings, EveryLocationIsUniqueAndInTheRightSection)
{
  std::set<std::string> seen_keys;

  for (const Config::RemixSettingMeta& meta : Config::GetRemixSettingsMetadata())
  {
    const Config::Location& location = RowLocation(meta);

    EXPECT_EQ(location.system, Config::System::GFX) << location.key;
    EXPECT_EQ(location.section, "Settings") << location.key;
    EXPECT_TRUE(location.key.starts_with("Remix")) << location.key;
    EXPECT_TRUE(seen_keys.insert(location.key).second) << "duplicate row for " << location.key;
  }

  // Unique keys plus the compile-time count check means the table is a
  // bijection onto the declarations.
  EXPECT_EQ(seen_keys.size(), Config::kRemixSettingCount);
}

TEST(RemixSettings, EveryRowIsDescribed)
{
  for (const Config::RemixSettingMeta& meta : Config::GetRemixSettingsMetadata())
  {
    const Config::Location& location = RowLocation(meta);

    ASSERT_NE(meta.tooltip, nullptr) << location.key;
    EXPECT_NE(std::string{meta.tooltip}, "") << location.key;
  }
}

TEST(RemixSettings, NumericRowsHaveAUsableRange)
{
  for (const Config::RemixSettingMeta& meta : Config::GetRemixSettingsMetadata())
  {
    const Config::Location& location = RowLocation(meta);

    const bool is_numeric = std::holds_alternative<const Config::Info<int>*>(meta.setting) ||
                            std::holds_alternative<const Config::Info<float>*>(meta.setting);
    if (!is_numeric)
      continue;

    EXPECT_LT(meta.min, meta.max) << location.key;
    EXPECT_GT(meta.step, 0.0f) << location.key;
  }
}

TEST(RemixSettings, OnlyIntegerRowsOfferChoices)
{
  for (const Config::RemixSettingMeta& meta : Config::GetRemixSettingsMetadata())
  {
    const Config::Location& location = RowLocation(meta);

    if (meta.choices.empty())
      continue;

    EXPECT_TRUE(std::holds_alternative<const Config::Info<int>*>(meta.setting)) << location.key;
    // The stored value is the combo index, so a one-entry combo cannot express
    // anything and every entry has to be real text.
    EXPECT_GE(meta.choices.size(), 2u) << location.key;
    for (const char* const choice : meta.choices)
    {
      ASSERT_NE(choice, nullptr) << location.key;
      EXPECT_NE(std::string{choice}, "") << location.key;
    }
  }
}
