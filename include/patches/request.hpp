// Request patch: replace the game's encrypted-transport with plain HTTP via WinHTTP.
// Mirrors Myosotis/Patches/Request.cs.
//
// Two phases:
//   (1) Init scan: walk Assembly-CSharp for every *Command type whose base is
//       HttpApiSchema<TReq,TResp>. Instantiate TResp, invoke its get_PacketId,
//       build a map from apiPath -> packetId. apiPath = "/" + ApiClass.ToLower() + ApiPath.
//   (2) Hook HttpApiRequester.AddRequest(HttpApiSchema): invoke the schema's
//       parameterless URL and body getters (the C# version finds them by
//       signature: returns a string starting with "http" / "{\"" respectively),
//       rewrite the URL host to config.server, POST via WinHTTP synchronously,
//       rewrite "packetId": <n> in the response, invoke the schema's
//       _responseEvent.Invoke(text).
//
// The synchronous WinHTTP approach blocks the game thread for the round-trip.
// For a private/local server this is acceptable and lets us avoid coroutines
// entirely (no MonoBehaviour injection needed from native C++).

#pragma once
namespace myosotis::patches {
bool install_request();
}
