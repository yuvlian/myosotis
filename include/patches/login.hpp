// Login patch: hardcode the steam token and bypass the browser-login dance.
// Mirrors Myosotis/Patches/Login.cs + Server.cs + the SteamID/ticket hooks.
//
// Hooks (via methodPointer overwrite):
//   - Steamworks.SteamClient.Init(AppId)         -> no-op, mark initialized
//   - Steamworks.ISteamUser.GetSteamID()          -> return a stable random-ish SteamId
//   - Steamworks.SteamUser.GetAuthSessionTicket() -> return AuthTicket with Data = UTF8(token)
//   - Login.PlayerPrefs.GetInt(key, def)          -> 1 (GUEST) for account-related keys
//   - LoginInfoManager.ProviderLogin_Steam()      -> no-op
//   - LoginInfoManager.StartSendPresence()        -> no-op
//   - LoginInfoManager.StopSendPresence()         -> no-op
//
// The hardcoded token comes from config.token. It's the same value the C# version
// would have obtained via the browser flow + /auth/token/poll + /login/SignInAsSteam
// — we skip all of that and feed it straight into the auth-ticket path.

#pragma once
namespace myosotis::patches {
bool install_login();
}
