#include "game/account_manager.hpp"

#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

namespace opm::server {

AccountManager::AccountManager(const std::string& accountsPath)
    : accountsPath_(accountsPath)
{
}

bool AccountManager::initialize()
{
    if (!loadFromFile()) {
        // File doesn't exist or failed to parse; create default admin
        std::cout << "[accounts] creating new accounts file with default admin\n";
        createDefaultAdmin();
        if (!saveToFile()) {
            std::cerr << "[accounts] failed to save accounts file\n";
            return false;
        }
    }
    return true;
}

std::optional<AuthToken> AccountManager::login(const std::string& username, const std::string& password)
{
    auto it = accounts_.find(username);
    if (it == accounts_.end()) {
        std::cout << "[accounts] login failed: unknown user '" << username << "'\n";
        return std::nullopt;
    }

    const auto& account = it->second;
    // Simple password check (in production, use proper hashing)
    if (account.passwordHash != password) {
        std::cout << "[accounts] login failed: wrong password for '" << username << "'\n";
        return std::nullopt;
    }

    // Generate token
    std::string token = generateToken();
    tokens_[token] = username;

    std::cout << "[accounts] login success: '" << username << "' (display: '" << account.displayName << "')\n";
    return AuthToken{token, username, account.displayName};
}

std::optional<AuthToken> AccountManager::validateToken(const std::string& token)
{
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        return std::nullopt;
    }

    const auto& username = it->second;
    auto accountIt = accounts_.find(username);
    if (accountIt == accounts_.end()) {
        // Token references deleted account; invalidate token
        tokens_.erase(it);
        return std::nullopt;
    }

    return AuthToken{token, username, accountIt->second.displayName};
}

bool AccountManager::updateDisplayName(const std::string& username, const std::string& displayName)
{
    auto it = accounts_.find(username);
    if (it == accounts_.end()) {
        return false;
    }

    it->second.displayName = displayName;
    return saveToFile();
}

std::optional<Account> AccountManager::getAccount(const std::string& username) const
{
    auto it = accounts_.find(username);
    if (it == accounts_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool AccountManager::save()
{
    return saveToFile();
}

std::string AccountManager::generateToken()
{
    static std::mt19937 rng{std::random_device{}()};
    static std::uniform_int_distribution<int> dist(0, 61);
    static constexpr const char* chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) {
        token += chars[dist(rng)];
    }
    return token;
}

void AccountManager::createDefaultAdmin()
{
    Account admin;
    admin.username = "super";
    admin.passwordHash = "super";  // Simple password hash (should use bcrypt/scrypt in production)
    admin.displayName = "Administrator";
    accounts_["super"] = admin;
}

bool AccountManager::loadFromFile()
{
    std::ifstream file(accountsPath_);
    if (!file.is_open()) {
        return false;
    }

    try {
        // Simple line-based format: username:hash:displayname
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') {
                continue;  // Skip empty lines and comments
            }

            size_t pos1 = line.find(':');
            if (pos1 == std::string::npos) {
                continue;  // Invalid line
            }

            size_t pos2 = line.find(':', pos1 + 1);
            if (pos2 == std::string::npos) {
                continue;  // Invalid line
            }

            Account account;
            account.username = line.substr(0, pos1);
            account.passwordHash = line.substr(pos1 + 1, pos2 - pos1 - 1);
            account.displayName = line.substr(pos2 + 1);

            accounts_[account.username] = account;
        }

        std::cout << "[accounts] loaded " << accounts_.size() << " account(s)\n";
        return true;
    } catch (...) {
        std::cerr << "[accounts] failed to parse accounts file\n";
        return false;
    }
}

bool AccountManager::saveToFile() const
{
    std::ofstream file(accountsPath_);
    if (!file.is_open()) {
        std::cerr << "[accounts] failed to open accounts file for writing\n";
        return false;
    }

    try {
        file << "# User accounts file - format: username:password:displayname\n";
        for (const auto& [username, account] : accounts_) {
            file << account.username << ":" << account.passwordHash << ":" << account.displayName << "\n";
        }
        file.flush();
        std::cout << "[accounts] saved " << accounts_.size() << " account(s)\n";
        return true;
    } catch (...) {
        std::cerr << "[accounts] failed to write accounts file\n";
        return false;
    }
}

} // namespace opm::server
