// Http patch: redirect POST + rewrite serverinfos_* requests.
// Mirrors Myosotis/Patches/Http.cs minus the CDN-redirect/CDN-password parts.
//
// Two hooks:
//   - UnityWebRequest.Post(string uri, string postData) prefix:
//       rewrite the uri's host/scheme to config.server.
//   - UnityWebRequest.SendWebRequest() prefix:
//       - if url host is notice.limbuscompanyapi.com -> rewrite to config.server host
//       - if url absolutePath starts with /serverinfos_ with no extra slash -> rewrite
//         to config.serverinfos_url
//
// We don't touch download.limbuscompanycdn.org / d7g8h56xas73g.cloudfront.net
// (the user said CDN redirect isn't needed) and we don't inject X-Requested-With
// (the CDN password is gone).
//
// Implementation note: UnityWebRequest is a managed object. Rewriting its `url`
// means setting the `url` property (a managed setter) via il2cpp_runtime_invoke,
// since the underlying field is private and its offset isn't stable. We resolve
// the `set_url` method and invoke it with a managed string.

#pragma once
namespace myosotis::patches {
bool install_http();
}
