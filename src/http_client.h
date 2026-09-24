#pragma once
#include <string>

struct TokenResponse {
  std::string token;
  std::string livekit_url;
  std::string identity;
  std::string display_name;
};

TokenResponse fetch_streamer_token(
    const std::wstring& base_url,
    const std::string& username,
    const std::string& password);
