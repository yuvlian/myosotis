import json
import uvicorn
from fastapi import FastAPI, Request, Response
from fastapi.responses import JSONResponse

app = FastAPI()

# --- Config and Mock Data ---
CANNED_SERVERINFOS = json.loads("""[
  {
    "platform": "android",
    "serviceType": "product",
    "serverId": "aos_product",
    "versions": ["1.109.1"],
    "serverUrl": "https://www.limbuscompanyapi.com",
    "logServerUrl": "https://battlelog.limbuscompanyapi.com",
    "presenceServerUrl": "https://presence.limbuscompanyapi.com",
    "noticeUrl": "https://notice.limbuscompanyapi.com",
    "cdnUrl": "",
    "guestAuthErrors": [101,15003,40004,40005],
    "enablePacketCrypt": true,
    "enableLogPacketCrypt": true,
    "enableAgreementVersionCheck": true,
    "enableNetworkingUIProcessPurchase": true,
    "enableCheckUpdateCatalogSteamFixed": true,
    "enableCheckUpdateCatalogToTitle": false,
    "enableCheckUpdateTextAsset": true,
    "enableCheckError": true,
    "enableBLogSync": true
  },
  {
    "platform": "ios",
    "serviceType": "product",
    "serverId": "ios_product",
    "versions": ["1.109.1"],
    "serverUrl": "https://www.limbuscompanyapi.com",
    "logServerUrl": "https://battlelog.limbuscompanyapi.com",
    "presenceServerUrl": "https://presence.limbuscompanyapi.com",
    "noticeUrl": "https://notice.limbuscompanyapi.com",
    "cdnUrl": "",
    "guestAuthErrors": [101,15003,40004,40005],
    "enablePacketCrypt": true,
    "enableLogPacketCrypt": true,
    "enableAgreementVersionCheck": true,
    "enableNetworkingUIProcessPurchase": true,
    "enableCheckUpdateCatalogSteamFixed": true,
    "enableCheckUpdateCatalogToTitle": false,
    "enableCheckUpdateTextAsset": true,
    "enableBLogSync": true
  },
  {
    "platform": "windows",
    "serviceType": "product",
    "serverId": "win_product",
    "versions": ["1.109.1"],
    "serverUrl": "https://www.limbuscompanyapi.com",
    "logServerUrl": "https://battlelog.limbuscompanyapi.com",
    "presenceServerUrl": "https://presence.limbuscompanyapi.com",
    "noticeUrl": "https://notice.limbuscompanyapi.com",
    "cdnUrl": "",
    "guestAuthErrors": [101,15003,40004,40005],
    "enablePacketCrypt": true,
    "enableLogPacketCrypt": true,
    "enableAgreementVersionCheck": true,
    "enableNetworkingUIProcessPurchase": true,
    "enableCheckUpdateCatalogSteamFixed": true,
    "enableCheckUpdateCatalogToTitle": false,
    "enableCheckUpdateTextAsset": true,
    "enableCheckError": true,
    "enableBLogSync": true
  },
  {
    "platform": "android",
    "serviceType": "exam",
    "serverId": "aos_exam",
    "versions": ["1.110.0"],
    "serverUrl": "https://www.limbuscompanyapi-2.com",
    "logServerUrl": "https://battlelog.limbuscompanyapi-2.com",
    "noticeUrl": "https://notice.limbuscompanyapi-2.com",
    "cdnUrl": "",
    "guestAuthErrors": [101,15003,40004,40005],
    "enablePacketCrypt": true,
    "enableLogPacketCrypt": true,
    "enableAgreementVersionCheck": true,
    "enableNetworkingUIProcessPurchase": true,
    "enableCheckUpdateCatalogSteamFixed": true,
    "enableCheckUpdateCatalogToTitle": false,
    "enableCheckUpdateTextAsset": true,
    "enableCheckError": true,
    "enableBLogSync": true
  },
  {
    "platform": "ios",
    "serviceType": "exam",
    "serverId": "ios_exam",
    "versions": ["1.110.0"],
    "serverUrl": "https://www.limbuscompanyapi-2.com",
    "logServerUrl": "https://battlelog.limbuscompanyapi-2.com",
    "noticeUrl": "https://notice.limbuscompanyapi-2.com",
    "cdnUrl": "",
    "guestAuthErrors": [101,15003,40004,40005],
    "enablePacketCrypt": true,
    "enableLogPacketCrypt": true,
    "enableAgreementVersionCheck": true,
    "enableNetworkingUIProcessPurchase": true,
    "enableCheckUpdateCatalogSteamFixed": true,
    "enableCheckUpdateCatalogToTitle": false,
    "enableCheckUpdateTextAsset": true,
    "enableBLogSync": true
  },
  {
    "platform": "windows",
    "serviceType": "exam",
    "serverId": "win_exam",
    "versions": ["1.110.0"],
    "serverUrl": "https://www.limbuscompanyapi-2.com",
    "logServerUrl": "https://battlelog.limbuscompanyapi-2.com",
    "noticeUrl": "https://notice.limbuscompanyapi-2.com",
    "cdnUrl": "",
    "guestAuthErrors": [101,15003,40004,40005],
    "enablePacketCrypt": true,
    "enableLogPacketCrypt": true,
    "enableAgreementVersionCheck": true,
    "enableNetworkingUIProcessPurchase": true,
    "enableCheckUpdateCatalogSteamFixed": true,
    "enableCheckUpdateCatalogToTitle": false,
    "enableCheckUpdateTextAsset": true,
    "enableCheckError": true,
    "enableBLogSync": true
  }
]""")

# --- Middleware/Logger ---
@app.middleware("http")
async def log_requests(request: Request, call_next):
    body = await request.body()
    print(f"\n--- {request.method} {request.url.path} from {request.client.host}:{request.client.port}")
    for k, v in request.headers.items():
        print(f"    {k}: {v}")
    
    if body:
        try:
            pretty = json.dumps(json.loads(body), indent=2, ensure_ascii=False)
            print("    body (json):")
            for line in pretty.splitlines():
                print(f"      {line}")
        except Exception:
            print(f"    body (raw, {len(body)} bytes): {body[:200]!r}")
    else:
        print("    body: <empty>")
        
    return await call_next(request)

# --- Routes ---
@app.get("/serverinfos.json")
@app.get("/serverinfos_{suffix}")
async def get_serverinfos():
    return JSONResponse(content=CANNED_SERVERINFOS)

@app.post("/login/CheckClientVersion")
async def check_client_version():
    print("SENT RESPONSE!")
    content = {
        "serverInfo": {"version": "product"},
        "state": "ok",
        "result": {"timeoffset": 0},
        "packetId": 33
    }
    return JSONResponse(
        content=content,
        headers={"content-encrypted": "0"}
    )

@app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"])
async def default_handler():
    return {}

if __name__ == "__main__":
    uvicorn.run(app, host="127.0.0.1", port=3000)
