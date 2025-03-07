// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/chrome/browser/passwords/account_select_fill_data.h"

#include "base/strings/utf_string_conversions.h"
#include "components/autofill/core/common/password_form_fill_data.h"
#include "ios/chrome/browser/passwords/test_helpers.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"

using autofill::PasswordFormFillData;
using password_manager::AccountSelectFillData;
using password_manager::FillData;
using password_manager::UsernameAndRealm;
using test_helpers::SetPasswordFormFillData;

namespace {
// Test data.
const char* kUrl = "http://example.com/";
const char* kFormNames[] = {"form_name1", "form_name2"};
const char* kUsernameElements[] = {"username1", "username2"};
const char* kUsernames[] = {"user0", "u_s_e_r"};
const char* kPasswordElements[] = {"password1", "password2"};
const char* kPasswords[] = {"password0", "secret"};
const char* kAdditionalUsernames[] = {"u$er2", nullptr};
const char* kAdditionalPasswords[] = {"secret", nullptr};

class AccountSelectFillDataTest : public PlatformTest {
 public:
  AccountSelectFillDataTest() {
    for (size_t i = 0; i < arraysize(form_data_); ++i) {
      SetPasswordFormFillData(form_data_[i], kUrl, kUrl, kUsernameElements[i],
                              kUsernames[i], kPasswordElements[i],
                              kPasswords[i], kAdditionalUsernames[i],
                              kAdditionalPasswords[i], false);

      form_data_[i].name = base::ASCIIToUTF16(kFormNames[i]);
    }
  }

 protected:
  PasswordFormFillData form_data_[2];
};

TEST_F(AccountSelectFillDataTest, EmptyReset) {
  AccountSelectFillData account_select_fill_data;
  EXPECT_TRUE(account_select_fill_data.Empty());

  account_select_fill_data.Add(form_data_[0]);
  EXPECT_FALSE(account_select_fill_data.Empty());

  account_select_fill_data.Reset();
  EXPECT_TRUE(account_select_fill_data.Empty());
}

TEST_F(AccountSelectFillDataTest, IsSuggestionsAvailableOneForm) {
  AccountSelectFillData account_select_fill_data;
  account_select_fill_data.Add(form_data_[0]);

  // Suggestions are avaialable for the correct form and field names.
  EXPECT_TRUE(account_select_fill_data.IsSuggestionsAvailable(
      form_data_[0].name, form_data_[0].username_field.name));
  // Suggestions are not avaialable for different form name.
  EXPECT_FALSE(account_select_fill_data.IsSuggestionsAvailable(
      form_data_[0].name + base::ASCIIToUTF16("1"),
      form_data_[0].username_field.name));

  // Suggestions are not avaialable for different field name.
  EXPECT_FALSE(account_select_fill_data.IsSuggestionsAvailable(
      form_data_[0].name,
      form_data_[0].username_field.name + base::ASCIIToUTF16("1")));
}

TEST_F(AccountSelectFillDataTest, IsSuggestionsAvailableTwoForms) {
  AccountSelectFillData account_select_fill_data;
  account_select_fill_data.Add(form_data_[0]);
  account_select_fill_data.Add(form_data_[1]);

  // Suggestions are avaialable for the correct form and field names.
  EXPECT_TRUE(account_select_fill_data.IsSuggestionsAvailable(
      form_data_[0].name, form_data_[0].username_field.name));
  // Suggestions are avaialable for the correct form and field names.
  EXPECT_TRUE(account_select_fill_data.IsSuggestionsAvailable(
      form_data_[1].name, form_data_[1].username_field.name));
  // Suggestions are not avaialable for different form name.
  EXPECT_FALSE(account_select_fill_data.IsSuggestionsAvailable(
      form_data_[0].name + base::ASCIIToUTF16("1"),
      form_data_[0].username_field.name));
}

TEST_F(AccountSelectFillDataTest, RetrieveSuggestionsOneForm) {
  AccountSelectFillData account_select_fill_data;
  account_select_fill_data.Add(form_data_[0]);

  std::vector<UsernameAndRealm> suggestions =
      account_select_fill_data.RetrieveSuggestions(
          form_data_[0].name, form_data_[0].username_field.name,
          base::string16());
  EXPECT_EQ(2u, suggestions.size());
  EXPECT_EQ(base::ASCIIToUTF16(kUsernames[0]), suggestions[0].username);
  EXPECT_EQ(std::string(), suggestions[0].realm);
  EXPECT_EQ(base::ASCIIToUTF16(kAdditionalUsernames[0]),
            suggestions[1].username);
  EXPECT_EQ(std::string(), suggestions[1].realm);
}

TEST_F(AccountSelectFillDataTest, RetrieveSuggestionsTwoForm) {
  // Test that after adding two PasswordFormFillData, suggestions for both forms
  // are shown, with credentials from the second PasswordFormFillData. That
  // emulates the case when credentials in the Password Store were changed
  // between load the first and the second forms.
  AccountSelectFillData account_select_fill_data;
  account_select_fill_data.Add(form_data_[0]);
  account_select_fill_data.Add(form_data_[1]);

  std::vector<UsernameAndRealm> suggestions =
      account_select_fill_data.RetrieveSuggestions(
          form_data_[0].name, form_data_[0].username_field.name,
          base::string16());
  EXPECT_EQ(1u, suggestions.size());
  EXPECT_EQ(base::ASCIIToUTF16(kUsernames[1]), suggestions[0].username);

  suggestions = account_select_fill_data.RetrieveSuggestions(
      form_data_[1].name, form_data_[1].username_field.name, base::string16());
  EXPECT_EQ(1u, suggestions.size());
  EXPECT_EQ(base::ASCIIToUTF16(kUsernames[1]), suggestions[0].username);
}

TEST_F(AccountSelectFillDataTest, RetrieveSuggestionsPrefix) {
  // Test that after adding two PasswordFormFillData, suggestions for both forms
  // are shown, with credentials from the second PasswordFormFillData. That
  // emulates the case when credentials in the Password Store were changed
  // between load the first and the second forms.
  AccountSelectFillData account_select_fill_data;
  account_select_fill_data.Add(form_data_[0]);

  std::vector<UsernameAndRealm> suggestions =
      account_select_fill_data.RetrieveSuggestions(
          form_data_[0].name, form_data_[0].username_field.name,
          base::ASCIIToUTF16("u"));
  // u prefix of username and additional username.
  EXPECT_EQ(2u, suggestions.size());
  EXPECT_EQ(base::ASCIIToUTF16(kUsernames[0]), suggestions[0].username);
  EXPECT_EQ(base::ASCIIToUTF16(kAdditionalUsernames[0]),
            suggestions[1].username);

  suggestions = account_select_fill_data.RetrieveSuggestions(
      form_data_[0].name, form_data_[0].username_field.name,
      base::ASCIIToUTF16("u$"));
  // u$ is prefix of additional username.
  EXPECT_EQ(1u, suggestions.size());
  EXPECT_EQ(base::ASCIIToUTF16(kAdditionalUsernames[0]),
            suggestions[0].username);

  suggestions = account_select_fill_data.RetrieveSuggestions(
      form_data_[0].name, form_data_[0].username_field.name,
      base::ASCIIToUTF16("1"));
  // 1 is prefix of neither usernames.
  EXPECT_TRUE(suggestions.empty());
}

TEST_F(AccountSelectFillDataTest, RetrievePSLMatchedSuggestions) {
  AccountSelectFillData account_select_fill_data;
  const char* kRealm = "http://a.example.com/";
  std::string kAdditionalRealm = "http://b.example.com/";

  // Make logins to be PSL matched by setting non-empy realm.
  form_data_[0].preferred_realm = kRealm;
  form_data_[0].additional_logins.begin()->second.realm = kAdditionalRealm;

  account_select_fill_data.Add(form_data_[0]);
  std::vector<UsernameAndRealm> suggestions =
      account_select_fill_data.RetrieveSuggestions(
          form_data_[0].name, form_data_[0].username_field.name,
          base::string16());
  EXPECT_EQ(2u, suggestions.size());
  EXPECT_EQ(base::ASCIIToUTF16(kUsernames[0]), suggestions[0].username);
  EXPECT_EQ(kRealm, suggestions[0].realm);
  EXPECT_EQ(base::ASCIIToUTF16(kAdditionalUsernames[0]),
            suggestions[1].username);
  EXPECT_EQ(kAdditionalRealm, suggestions[1].realm);
}

TEST_F(AccountSelectFillDataTest, GetFillData) {
  AccountSelectFillData account_select_fill_data;
  account_select_fill_data.Add(form_data_[0]);
  account_select_fill_data.Add(form_data_[1]);

  for (size_t form_i = 0; form_i < arraysize(form_data_); ++form_i) {
    const auto& form_data = form_data_[form_i];
    // GetFillData() doesn't have form identifier in arguments, it should be
    // provided in RetrieveSuggestions().
    account_select_fill_data.RetrieveSuggestions(
        form_data.name, form_data.username_field.name, base::string16());

    std::unique_ptr<FillData> fill_data =
        account_select_fill_data.GetFillData(base::ASCIIToUTF16(kUsernames[1]));

    ASSERT_TRUE(fill_data);
    EXPECT_EQ(form_data.origin, fill_data->origin);
    EXPECT_EQ(form_data.action, fill_data->action);
    EXPECT_EQ(base::ASCIIToUTF16(kUsernameElements[form_i]),
              fill_data->username_element);
    EXPECT_EQ(base::ASCIIToUTF16(kUsernames[1]), fill_data->username_value);
    EXPECT_EQ(base::ASCIIToUTF16(kPasswordElements[form_i]),
              fill_data->password_element);
    EXPECT_EQ(base::ASCIIToUTF16(kPasswords[1]), fill_data->password_value);
  }
}

TEST_F(AccountSelectFillDataTest, GetFillDataOldCredentials) {
  AccountSelectFillData account_select_fill_data;
  account_select_fill_data.Add(form_data_[0]);
  account_select_fill_data.Add(form_data_[1]);

  // GetFillData() doesn't have form identifier in arguments, it should be
  // provided in RetrieveSuggestions().
  account_select_fill_data.RetrieveSuggestions(
      form_data_[0].name, form_data_[0].username_field.name, base::string16());

  // AccountSelectFillData should keep only last credentials. Check that in
  // request of old credentials nothing is returned.
  std::unique_ptr<FillData> fill_data =
      account_select_fill_data.GetFillData(base::ASCIIToUTF16(kUsernames[0]));
  EXPECT_FALSE(fill_data);
}

}  // namespace
