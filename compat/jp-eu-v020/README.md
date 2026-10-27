# BBHost 日本語プレイ・欧州オンライン接続 統合パッケージ

**対応：BBHost 0.2.20 系の検証済みカスタムHTTPソース／Windows x64・Linux x86-64／欧州GOTY版・日本版（試験版）**

この配布物は、従来の地域変更プラグインと、タイトル画面へ戻った後の再接続・プレイログの不正なS3送信先に対応する**最新の修正**を、１つのZIPにまとめたものです。日本語表示を維持したまま、オンライン処理で欧州向け設定を使用します。接続先は非公式の The Hunter's Dream サーバーであり、PlayStation Network ではありません。

**先に読むこと：** `host-http/install_jp_eu_v020_fix.py` は、今回検証した特定のカスタム `src/hle/http.cpp` にしか適用できません。完全に未改造のBBHostや、別の更新版に直接適用できるものではありません。ハッシュが一致しない場合は何も上書きせず中止します。**以前の `install_http_compat.py` は同梱していません。旧版を再適用すると再接続の修正が失われる場合があります。**

## 内容

| フォルダー／ファイル | 用途 |
|---|---|
| `linux/eu-goty/region_switch.so` | 欧州GOTY版用・Linux |
| `windows/eu-goty/region_switch.dll` | 欧州GOTY版用・Windows |
| `linux/jp-old-hunters/region_switch.so` | 日本版用・Linux |
| `windows/jp-old-hunters/region_switch.dll` | 日本版用・Windows |
| `host-http/install_jp_eu_v020_fix.py` | **最新の再接続・S3対応の安全確認／適用** |
| `host-http/http-jp-eu-v020-fixed.cpp` | 上の適用プログラムが使用する修正済みソース |
| `host-http/bbhost-v020-reconnect-s3-fix.patch` | 修正内容を確認するための差分（自動適用はしない） |
| `install_plugin.py` | 対応プラグインの配置補助（旧版をバックアップ） |
| `check_japanese_dump.py` | 日本版の構成を検査する補助プログラム |
| `src/`、`tests/` | プラグインのソースコードと模擬試験用コード |
| `SHA256SUMS.txt` | 各ファイルの整合性確認 |

## １．ゲームデータの種類を確認

- 欧州GOTY版 `CUSA03173`、原本の `USER_DEFINED_PARAM_1=13` → `eu-goty` を選ぶ。
- 日本版 Old Hunters `CUSA03014`、`USER_DEFINED_PARAM_1=0` → `jp-old-hunters` を選ぶ。
- **両方のプラグインを同時に設置しない。** 使用するOSとゲーム版に合うもの１つだけを使用する。
- BBHostが要求する復号済み1.09実行ファイルのSHA-256は `941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7`。一致しない実行ファイルのチェックを無効化しない。日本版独自の実行ファイルとゲームデータの組合せは未検証。

## ２．既存環境をバックアップ

BBHostの本体・設定・セーブデータをバックアップする。以前の `region_switch.dll`／`.so` と、修正前の `src/hle/http.cpp` も残しておく。以前導入した `jp-eu-online-selectors.toml` はプラグイン内の12か所のパッチと重複するため、退避する。ゲームファイルそのものを直接書き換える必要はない。

## ３．プラグインを配置

**Linux の例（欧州GOTY版）：** ZIPを展開し、展開先へ移動してから実行する。配置先は、実際に起動しているBBHostがプラグインを読み込むディレクトリを指定する。

```bash
python3 install_plugin.py --edition eu --plugins-dir ~/Documents/bbhost/build/plugins --dry-run
python3 install_plugin.py --edition eu --plugins-dir ~/Documents/bbhost/build/plugins
```

**Windows の例（日本版）：** PowerShellで展開先から実行する。

```powershell
py -3 .\install_plugin.py --edition jp --platform windows --plugins-dir "C:\Games\bbhost\plugins" --dry-run
py -3 .\install_plugin.py --edition jp --platform windows --plugins-dir "C:\Games\bbhost\plugins"
```

配置補助を使わず、対応する `region_switch.dll`／`.so` を直接コピーしてもよい。過去の同名プラグインは先に退避すること。

`bbhost.toml` は、既存の表を重複させずに次を設定する。

```toml
[plugins]
region_switch = true
signed_only = false

[region_switch]
region = "JP"
```

## ４．最新のHTTP修正を適用（BBHost 0.2.20の既存カスタム環境）

**プラグインだけでHTTP処理の修正は完結しない。** BBHost本体の再ビルドが必要。LinuxとWindowsで同じ `http.cpp` を使用する。

Linuxでは：

```bash
python3 host-http/install_jp_eu_v020_fix.py --source ~/Documents/bbhost --dry-run
python3 host-http/install_jp_eu_v020_fix.py --source ~/Documents/bbhost
cmake --build ~/Documents/bbhost/build -j4
```

Windowsでは、Windows用BBHostのソースツリーに対してPowerShellから：

```powershell
py -3 .\host-http\install_jp_eu_v020_fix.py --source "C:\src\bbhost" --dry-run
py -3 .\host-http\install_jp_eu_v020_fix.py --source "C:\src\bbhost"
```

その後、BBHost公式のWindowsビルド手順で `bbhost.exe` を再ビルドする。実行ファイルの置き換えは、BBHostを終了してから行う。

- 確認結果が「Already installed」であれば同じ修正済みファイルが既に存在する。
- 確認結果が「Unexpected http.cpp SHA-256」であれば**作業を中止**する。別バージョンのソースを無理に上書きしない。
- 以前の単発HTTPブートストラップを適用していた環境でのみ、既知の旧ファイルから今回の修正済みファイルに安全に移行できる。
- **新規導入／異なるバージョン：** 以前の改造済みソースと一致しない場合、このインストーラーだけで新規セットアップはできない。該当バージョン用の統合修正が必要。`host-http/http-jp-eu-v020-fixed.cpp` を強制的に上書きコピーしないこと。

BBHostの公式ビルド資料：https://github.com/droogie/bbhost/blob/master/docs/building.md

## ５．接続と再接続を確認

BBHostを起動したら、初回ログイン→プレイ中の通常通信→タイトル画面へ戻る→プロセスを終了せず再ログイン、の順で確認する。修正済みソースは、空のURLに対する欧州サーバー状態確認を１秒に最大１回、直近１分間に最大12回まで許可する。プレイログの空のS3バケットに対する処理は、該当形式のPUTだけが対象。

表示されるログの例：

```text
patch: jp-eu-online-selectors: 12 of 12 sites written
jp-eu-unified: bootstrap EU ss.info request=... (bounded retry .../12)
jp-eu-unified: online login OK (bounded bootstrap retries available)
jp-eu-unified: repaired missing-bucket play-log PUT request=...
```

最初の12か所のパッチは、起動順によっては「すでに書き込み済み」と表示される。HTTP 200だけで全オンライン機能の正常動作まで確認できたことにはならない。

## ６．元に戻す

1. BBHostを終了する。
2. `region_switch.dll`／`.so` を退避し、以前のプラグインが必要なら復元する。
3. `bbhost.toml` で `region_switch=false` にする。
4. `src/hle/http.cpp` に作成された日時入りの `.bak` を復元して再ビルドする。または、以前の動作確認済みBBHost本体を復元する。
5. 必要なら古いTOMLパッチを戻す（同じ箇所のパッチを重複させない）。

## ７．試験範囲と注意

- 欧州GOTY版・Linuxでは、利用者から最近の再接続修正後に「動作しているようだ」との報告がある。ただし長時間の安定性はまだ未確認。
- Windows版・日本版の実環境でのオンライン接続は未検証。ソース・ビルド成果物と実環境の動作は別の確認が必要。
- ビルド済みDLL/SOとテスト用コードを含むが、**Bloodborne本体・ゲームデータ・復号済みeboot・セーブデータ・ログやアカウント情報は一切同梱しない。**
- BBHostの更新時にはHTTPソースが変わる可能性がある。ハッシュ照合を外しての強制上書きは行わない。
