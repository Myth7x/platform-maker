#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opm::server {

// Account data stored in the accounts file
struct Account {
    std::string username {};      // unique login name
    std::string passwordHash {};  // hashed password (for now, just plain text)
    std::string displayName {};   // player name shown online
};

// Session token for an authenticated user
struct AuthToken {
    std::string token {};           // unique token string
    std::string username {};        // associated username
    std::string displayName {};     // associated display name
};

// Manages user accounts and authentication
class AccountManager {
public:
    explicit AccountManager(const std::string& accountsPath = "accounts.json");

    // Load accounts from file, creating default admin if needed
    bool initialize();

    // Authenticate a user. Returns token if successful.
    std::optional<AuthToken> login(const std::string& username, const std::string& password);

    // Validate an existing token
    [[nodiscard]] std::optional<AuthToken> validateToken(const std::string& token);

    // Update a user's display name
    bool updateDisplayName(const std::string& username, const std::string& displayName);

    // Get account info by username
    [[nodiscard]] std::optional<Account> getAccount(const std::string& username) const;

    // Save all accounts to file
    bool save();

private:
    std::string accountsPath_;
    std::unordered_map<std::string, Account> accounts_;  // username -> Account
    std::unordered_map<std::string, std::string> tokens_; // token -> username

    // Generate a random token
    [[nodiscard]] std::string generateToken();

    // Create default admin account
    void createDefaultAdmin();

    // Load/save helpers
    bool loadFromFile();
    bool saveToFile() const;
};

} // namespace opm::server
