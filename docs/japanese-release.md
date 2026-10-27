# bbhost 日本語版の使い方

bbhost 0.2.21 を基に、日本版 Bloodborne The Old Hunters（CUSA03014）専用の、日本語のみの Windows x64 / Linux x86-64 版です。日本版のオンライン動作は利用者により確認済みです（2026年10月27日）。接続先は非公式の The Hunter's Dream サーバーです。

## 対応するゲーム版

通常の Windows / Linux 配布物（`jp-only`）は、元の `sce_sys/param.sfo` が日本版 Old Hunters の `TITLE_ID = CUSA03014`、`USER_DEFINED_PARAM_1 = 0` である場合にのみ起動します。欧州版、北米版、他の日本版、識別情報がない・壊れたデータは拒否します。別フォルダーの更新データにも同じ条件を適用し、版の混在を拒否します。

設定画面では理由を日本語で表示し、プレイとシェーダーの事前コンパイルを無効にします。コマンドライン・ヘッドレス起動でも、プラグインやゲームコードを読み込む前に同じ検査を行います。プラグインを無効にする、入れ替える、設定や環境変数を変える方法では解除できません。`BBHOST_ANY_EBOOT` による実行ファイルのハッシュ検査の回避も無効です。

ゲーム版は原本のメタデータで識別します。全ゲームファイルの真正性を証明するものではありません。個人用の EU GOTY 互換版は別の Linux 実行ファイル・アーカイブ（`eu-personal`）として用意します。

## 用意するもの

- 自分で用意したゲームデータと更新 1.09。`dvdroot_ps4` と `sce_sys/param.sfo` が必要です。
- bbhost が対応する復号済み 1.09 実行ファイル（ELF）。SHA-256 は次の値です。

```text
941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7
```

ゲームデータ、実行ファイル、セーブ、ログ、アカウントの認証情報は配布物に含みません。

## 起動

Windows は展開したフォルダーの `run-bbhost.bat`、Linux は `./run-bbhost.sh` を実行します。Linux で実行権限が失われた場合は `chmod +x bbhost run-bbhost.sh` を実行してください。

設定画面でゲームフォルダーと復号済み eboot を指定して「保存」「プレイ」を選びます。後から設定画面を開くには `bbhost --setup`（Windows は `bbhost.exe --setup`）を使います。オンラインは設定画面のアカウント欄でログインします。

日本版用 `region_switch` はビルドと同梱済みで、初期状態から有効です。日本版の地域情報を維持し、オンライン処理を欧州向けに切り替えます。本体には再接続とプレイログ送信先の修正を統合済みなので、HTTP インストーラーを実行する必要はありません。

既存設定で無効にしていた場合は、設定画面のプラグイン欄で有効にしてください。手動設定は、既存の同名セクションに次を追加します。

```toml
[plugins]
region_switch = true

[region_switch]
region = "JP"
```

署名のない開発ビルドで `plugins.signed_only = true` を使っている場合は、同梱プラグインが読み込まれません。配布元と内容を確認したうえで、開発ビルドではこの設定を無効にします。

## 操作と保存先

- `F10`: PC 設定。閉じると保存します。
- `F9`: プラグインメニュー。
- ゲーム内の「システム」: PC 設定、画面効果、カメラ、キー設定など。
- 日本版では `Space` が回避・ダッシュ（×）、`E` が調べる（○）です。
- `F12`: 問題のあるフレームを保存します。

通常の設定は Windows の `%APPDATA%\bbhost`、Linux の `~/.config/bbhost` にあります。セーブとキャッシュは通常、Windows の `%LOCALAPPDATA%\bbhost\data`、Linux の `~/.local/share/bbhost/data` に保存します。既存設定や `paths.data` の指定がある場合はその場所を使用します。更新前にはセーブをバックアップしてください。

画面表示は日本語ですが、設定キー・設定値・オンラインの識別子は既存版との互換性のため従来の表記を維持します。例えば `window_mode = "Fullscreen"`、`model_detail = "Normal"` です。技術ログ、第三者のエラー文、外部サーバーの応答は元の表記の場合があります。

## 0.2.21 のパフォーマンス表示

F10 の「パフォーマンス表示」を有効にすると、画面の右上に処理状況を表示します。設定ファイルでは従来どおり `perf_overlay = "On"` として保存します。

- 「制限要因」: 現在の FPS を主に制限している処理。「次点」はその次に負荷が高い処理です。
- メインスレッド・GX ワーカー・描画スレッド: ゲーム側の処理と、並列処理の待ち時間です。
- コマンド処理・コマンド記録・GPU・画面表示: 描画の準備から表示までの各段階です。
- 「FPS 上限」は設定したフレームレートや垂直同期による制限、「ロード・待機」は読み込みなどによる長いフレームを示します。

下段にはメイン処理・GX・待機の時間（ms）と、CP（コマンド処理）・GPU の負荷を表示します。起動直後は測定が終わるまで「測定中」と表示します。初期状態は無効です。

0.2.21 の描画キュー、パーティクル、AMD 向け深度処理、切断後の接続処理は維持しています。ログ解析ツールが読む `perf:` や `perf summary:`、処理の識別子は変更していません。

## 同梱の互換パック

`compat/jp-eu-v020/` は、提供された `bbhost_jp_eu_v020_complete` 全体をそのまま保存したものです。両 OS・両ゲーム版のプラグイン、ソース、導入補助、模擬試験、HTTP 修正、説明書、チェックサムを含みます。`SHA256SUMS.txt` で原本との一致を検査できます。

原本の説明書には作成時点の「日本版は未検証」という記録が残っています。現在の日本版のオンライン動作は利用者により確認済みです。原本のハッシュを維持するため、当時の記録を改変していません。

原本には欧州版用プラグインも含まれていますが、本体の日本版専用制限を解除するものではありません。欧州 GOTY 版（CUSA03173）には、別途作成する個人用 Linux 配布物を使用します。古い `jp-eu-online-selectors.toml` は同じパッチと重複するため退避します。

原本の HTTP インストーラーは特定の旧カスタムソース専用です。この統合版には実行しません。今後の更新に対してハッシュ確認を外して適用しないでください。

## 動作確認と更新

この版には日本語フォントを内蔵しています。文字を表示するための追加フォントの導入は不要です。ゲームの文字や音声はゲームデータに含まれるものを使用します。

提供された修正では、タイトル画面からの再接続を可能にするため、対象の空 URL の GET に対する状態確認を１秒間隔・１分間に最大12回に制限しています。プレイログの補正は、空の S3 バケットを指定した所定の形式の PUT のみが対象です。

英語版の本体・プラグインで置き換えないよう、日本語版の更新先が設定されるまでは上流の自動取得を行いません。更新は日本語版の配布物を使用してください。外部サービスの可用性、回線や NAT、相手の MOD 構成は接続に影響します。

問題を報告するときは、OS、GPU、ゲーム版、発生した操作と日時を添えてください。ログに IP アドレスなどが含まれることがあるため、公開前に内容を確認してください。

## ビルド

Linux の通常ビルド:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Windows の Linux からのクロスビルド:

```sh
tools/win_deps.sh
cmake -S . -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake \
  -DCMAKE_PREFIX_PATH="$PWD/build/win-deps/prefix" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-win --target package-win -j4
```

Windows には Clang と mingw-w64 が必要です。Linux の配布用ビルドは `tools/linux_portable_build.sh` または Ubuntu 24.04 上の `tools/ci_build.sh` を使用します。通常のローカルビルドは、そのマシンの glibc、SDL3、FFmpeg などに依存します。実際の依存関係は Linux 配布物の `README-linux.txt` 末尾に記載されます。

CMake の既定値は `BBHOST_PERSONAL_EU=OFF`（日本版専用）です。個人用 EU 互換版は Linux でのみ `BBHOST_PERSONAL_EU=ON` を指定して別のビルド先に作成します。ポータブル版では `BBHOST_PERSONAL_EU=ON tools/linux_portable_build.sh` を実行すると `build-linux-portable/build-eu-personal/` に生成します。標準版のビルド先と混在しません。

パッケージ作成は `tools/package_linux.sh <bbhostのパス> <出力先>`、`tools/win_package.sh <bbhost.exeのパス> <出力先>` です。両方とも互換パック全体、プラグイン、日本語説明書、ライセンスを含めます。

## ライセンス

bbhost は GPL-3.0-or-later です。`LICENSE` を参照してください。M+ フォントのライセンスは `licenses/mplus/LICENSE`（ソースでは `third_party/mplus/LICENSE`）、DXC は `licenses/dxc/` にあります。その他の同梱コードには各ライセンスが適用されます。
