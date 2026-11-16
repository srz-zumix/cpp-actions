# cpp-actions

[English](README.md) | [日本語](README.ja.md)

GitHub Actions のワークフローをローカルで実行する C++20 製の CLI ツールです。
YAML の検証、ジョブの依存関係の表示、イベントで選択されるワークフローのプレビュー、
未コミットの変更を含むローカル作業ツリーでのジョブ実行ができます。

**サンドボックスでの実行には macOS が必要です。** ステップは既定で macOS Seatbelt を使用します。
VM や GitHub ホステッドランナーの代替環境ではなく、ホストにインストール済みのツールを使います。
Linux での実行には隔離の明示的な無効化とホストラベルの指定が必要です。
信頼できるコードのローカルデバッグに限って使用してください。

## 目次

- [ビルド](#ビルド)
- [クイックスタート](#クイックスタート)
- [ワークフローとジョブの選択](#ワークフローとジョブの選択)
- [アクセス権・環境変数・シークレットの設定](#アクセス権環境変数シークレットの設定)
- [結果の確認とクリーンアップ](#結果の確認とクリーンアップ)
- [ワークフローの互換性](#ワークフローの互換性)
- [セキュリティとトラブルシューティング](#セキュリティとトラブルシューティング)
- [詳細リファレンス](#詳細リファレンス)

## ビルド

CMake 3.24 以降、Ninja、C++20 対応コンパイラ、Git が必要です。
CMake はインストール済みの依存ライブラリを優先し、見つからなければ定義された依存ライブラリをダウンロードします。
cpp-actions のソースディレクトリで実行してください。

```sh
cmake --preset release
cmake --build --preset release
build/release/src/cpp-actions --help
```

`build/release/src/cpp-actions` を直接使うか、`PATH` に含まれる場所へインストールします。例:

```sh
cmake --install build/release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

開発・テスト用:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

サンドボックス内の Xcode 統合テストは明示的に有効化した場合のみ実行します。
Xcode、iOS SDK、利用可能な iOS Simulator ランタイムが必要です。
ホストの Simulator 基盤を起動・操作することがあり、実行には時間がかかる場合があります。

```sh
CPA_TEST_XCODE_STORYBOARD=1 build/debug/tests/cpa_unit_tests \
  --iutest_filter=ActionRunnerTest.XcodeStoryboardCompilesInSandbox
CPA_TEST_XCODE_ASSET_SYMBOLS=1 build/debug/tests/cpa_unit_tests \
  --iutest_filter=ActionRunnerTest.XcodeAssetSymbolsGeneratedInSandbox
CPA_TEST_XCODE_ASSET_CATALOG=1 build/debug/tests/cpa_unit_tests \
  --iutest_filter=ActionRunnerTest.XcodeAssetCatalogCompilesInSandbox
```

## クイックスタート

確認したいワークフローがあるリポジトリで実行します。
既定では `.github/workflows` からワークフローを探します。

```sh
# ワークフローの YAML を検証し、一覧を表示する。
cpp-actions validate
cpp-actions list

# 依存関係を確認し、push イベントを実行せずにプレビューする。
cpp-actions graph --workflow ci.yml --format dot
cpp-actions run --workflow ci.yml --event push --dry-run

# macOS で選択したワークフローを実行する。
cpp-actions run --workflow ci.yml --event push
```

`ci.yml` を対象のファイル名・パス・ワークフロー名に置き換え、`on:` のトリガーに対応するイベントを指定します。
イベントに一致しないワークフローはスキップされます。
`--dry-run` はステップを実行せず、実行用設定やシークレットも読み込まず、実行結果の出力先も作成しません。

ジョブは空のワークスペースで開始します。ワークフロー内で `actions/checkout` を使うと、
ローカルリポジトリのスナップショットが配置されます。
未コミットの変更・削除・無視対象でない未追跡ファイルも含まれ、元のリポジトリは変更しません。

macOS では macOS ランナーラベルが自動的に一致します。
その他のラベルには `--host-label` が必要です。ラベルをホストに対応付けるだけで、
OS のエミュレーションやツールのインストールはしません。
Linux では次の二つの明示的なオプションが必要です。

```sh
# 信頼できるコードのローカルデバッグ専用。ファイル・ネットワークの隔離はない。
cpp-actions run --workflow ci.yml --event push \
  --no-sandbox --host-label ubuntu-latest
```

**信頼できないワークフローやプルリクエストに `--no-sandbox` を使わないでください。**
ワークフローのコードは、最小限の環境変数に含まれない認証情報も含め、
実行ユーザーのファイルとネットワークにアクセスできます。
サンドボックスが利用できない場合でも、このモードに自動で切り替わることはありません。

## ワークフローとジョブの選択

| オプション | 用途 |
| --- | --- |
| `-C, --directory PATH` | 現在のディレクトリの代わりに対象リポジトリを指定する。 |
| `-W, --workflows PATH` | `.github/workflows` の代わりにファイルやディレクトリを指定する。 |
| `-w, --workflow SELECTOR` | 名前・ファイル名・パスで選択する（`run` / `graph`）。 |
| `-e, --event NAME` | シミュレートするイベントを指定する。 |
| `--event-path FILE` | JSON からイベントペイロードを読み込む。 |
| `--input NAME=VALUE` | `workflow_dispatch` の入力を指定する。ペイロードの値より優先される。 |
| `-j, --job ID` | 指定ジョブとその依存ジョブを実行する。 |
| `--no-needs` | 選択したジョブの依存ジョブを自動追加しない。 |
| `--matrix NAME=VALUE` | マトリクスの組み合わせを絞り込む。 |
| `--parallel N` | 並列ジョブ数の上限。既定の `0` は CPU 数を使う。 |
| `-n, --dry-run` | イベント選択とジョブを実行せずにプレビューする。 |

```sh
cpp-actions validate -C /path/to/repository
cpp-actions run --workflow ci.yml --job build --matrix configuration=debug
cpp-actions run --event workflow_dispatch --input target=debug
```

全オプションは `cpp-actions --help` と `cpp-actions run --help` で確認できます。

## アクセス権・環境変数・シークレットの設定

ステップには最小限の環境変数、ジョブ専用の `HOME` と `TMPDIR`、
ランナー・ワークフローの変数を渡します。
ホストの `PATH`、トークン、SSH エージェントなどは暗黙に継承しません。

信頼できるワークフローに必要なアクセスだけを許可してください。

```sh
cpp-actions run --workflow ci.yml \
  --allow-host registry.example.test \
  --allow-read "$HOME/sdk" \
  --pass-env SDKROOT --pass-env BUILD_MODE=debug

cpp-actions run --workflow ci.yml \
  --secret API_TOKEN \
  --secret-file "$HOME/.config/cpp-actions/secrets.env" \
  --var BUILD_MODE=debug
```

`registry.example.test` は架空のホスト名です。信頼するサービスに置き換えてください。
これらの権限・環境変数・シークレット・変数のオプションは繰り返し指定できます。
`--pass-env NAME` はホストの変数をコピーし、`NAME=VALUE` は値を直接指定します。
環境変数を渡しても、参照先ファイルの読み取りは**許可されません**。
別途 `--allow-read` を使い、ホームディレクトリ全体の許可は避けてください。

`--secret NAME` はホストの変数を読み込み、未設定ならエラーになります。
シークレットと変数は式のコンテキストで参照でき、ステップの環境変数には自動追加されません。
シークレットを引数やシェル履歴に残さないよう、環境変数やファイルからの読み込みを優先してください。
明示的に指定した `GITHUB_TOKEN` シークレットはアクション解決の認証にも使われ、
`--github-token`（別名 `--token`）がさらに優先されます。ホストのトークンは自動で読み込みません。

### 信頼済み設定

`run` はデフォルトで `$XDG_CONFIG_HOME/cpp-actions/config.yml`（XDG が未設定なら
`~/.config/cpp-actions/config.yml`）に続けて、選択したリポジトリルートの
`.cpp-actions.yml` を読み込みます。リポジトリ設定はセットアップステップの実行、
ホスト環境変数の取り込み、サンドボックス権限の緩和が可能なため、実行前に内容を確認し、
信頼できる場合にのみ使用してください。

```yaml
allow-hosts:
  - registry.example.test
allow-read:
  - ~/sdk
env-pass:
  - SDKROOT
parallel: 2
secrets-file: secrets.env
vars:
  BUILD_MODE: debug
```

各設定ファイル内のパスはそのファイルを基準とし、CLI のパスは現在のディレクトリ基準です。先頭の `~` は展開されます。
CLI の権限は設定の権限に追加され、CLI の単一値の設定や指定した値は設定ファイルより優先されます。
ユーザー設定がリポジトリ内に解決される場合は拒否されます。リポジトリ設定には `.cpp-actions.yml` を使用します。
`--config /trusted/path/config.yml` は自動設定を読み込まず、そのファイルのみを使用します。
リポジトリ内の `.env` とシークレットファイルは自動検出しません。
ドライランでは実行用設定を読み込みません。

環境別のシークレット・変数、セットアップステップなどは
[設定リファレンス（英語）](docs/reference.md#user-configuration)を参照してください。

## 結果の確認とクリーンアップ

既定では実行ごとに `<cache>/runs` 内に一意のディレクトリを作成します。
`<cache>` はプラットフォームのユーザーキャッシュディレクトリ内の `cpp-actions` です。
`--cache-dir` でキャッシュの場所を変更できます。
`--run-dir PATH` で指定する実行ディレクトリは、まだ存在しない必要があります。

ログと `report.json` は実行後も残ります。ステップがサマリーを出力した場合は `summary.md` も作成します。
アーティファクトは通常のファイルとして `<run>/artifacts`、
または `--artifact-dir PATH` で指定した別のルートに保存します。

ワークスペースは通常、実行後に削除します。`--keep-workdir` で調査用に保持できます。
実行ディレクトリの中ではなく、短いパスの個別の一時ディレクトリにあり、場所はログに表示されます。
保持したファイルにはシークレットが含まれる可能性があります。

```sh
cpp-actions clean --dry-run /path/to/run-dir
cpp-actions clean /path/to/run-dir
cpp-actions clean --all
```

`clean` は保持したワークスペース、Unity の一時ディレクトリ、実行ディレクトリ内の
`jobs`・`control`・`snapshot` を削除します。
ログ・レポート・サマリー・アーティファクトは残し、ロック中の実行には触れません。
中断して `report.json` がない実行には `--force` が必要です。
パスの検証や削除失敗時の動作は
[出力とクリーンアップのリファレンス（英語）](docs/reference.md#explicit-grants-and-inputs)を参照してください。

終了コードは成功・スキップが `0`、ワークフロー失敗が `1`、セットアップエラーが `2`、
キャンセルが `130` です。引数のエラーも非ゼロになります。
SIGINT/SIGTERM で正常なキャンセルを要求し、二度目のシグナルで強制キャンセルを要求します。
バックグラウンドサービスを後続ステップへ持ち越すことはできません。

## ワークフローの互換性

| 機能 | ローカルでの動作 |
| --- | --- |
| ジョブとステップ | `needs`、`if`、出力、マトリクス展開、fail-fast、max-parallel、continue-on-error、タイムアウトに対応。 |
| シェル | bash、sh、python、pwsh、`{0}` を含むカスタムコマンド。ホストにツールが必要。 |
| アクション | ローカル・リモートの JavaScript（`node20` / `node24`）と composite。pre/main/post に対応。 |
| `actions/checkout` | ローカル作業ツリーのスナップショットまたはリモートチェックアウト。サブモジュールと Git LFS に対応。 |
| キャッシュとアーティファクト | ローカルの組み込みストレージ。GitHub のサービスとは共有しない。 |
| 再利用可能ワークフロー | ローカル・リモート呼び出し、型付き入力、明示・継承シークレット、出力。最大 10 階層。 |
| 環境 | 設定したシークレット・変数。承認、保護ルール、デプロイ API は再現しない。 |
| `concurrency` | 評価・表示のみ。実行の直列化やキャンセルはしない。 |
| `permissions` | 警告のみ。渡したトークンの権限は縮小しない。 |
| コンテナと OIDC | container/service ジョブ、Docker コンテナアクション、OIDC サービスは非対応。 |

正確な動作は[アクションの互換性](docs/reference.md#actions)と
[ワークフローの実行](docs/reference.md#workflow-execution)を参照してください（詳細ガイドは英語）。
GitHub ホステッドランナーと同じ結果になる保証はありません。

## セキュリティとトラブルシューティング

Seatbelt は既定でアクセスを拒否し、必要なシステムソフトウェアと許可されたジョブ・キャッシュのパスに制限します。
ネットワークはジョブごとのホスト許可リスト付き HTTP/HTTPS プロキシを通ります。
GitHub と一部のパッケージ・ツール配布ホストは既定で許可されますが、別ホストへのリダイレクトには追加の許可が必要です。
プロキシ設定を無視するクライアントはブロックされ、自動転送されません。

| 症状 | 確認事項 |
| --- | --- |
| ワークフロー・ジョブが実行されない | イベントのトリガー、ジョブ条件、ホストラベル。`--dry-run` で確認する。 |
| ツールが見つからない | ホストへのインストールと、明示的な環境・読み取り権限の設定。 |
| ダウンロードが拒否される | 必要なホストとリダイレクト先の許可、クライアントのプロキシ対応。 |
| ホストファイルへのアクセスが拒否される | ホーム全体ではなく、狭い範囲の `--allow-read` を指定する。 |
| macOS の `actions/setup-python` が失敗する | [ホステッド Python の準備手順（英語）](docs/reference.md#actions)を読む。サンドボックス無効化で回避しない。 |
| 対話シェルが待ち続ける | 明示的に有効化する[非対話シェルモード（英語）](docs/reference.md#opt-in-non-interactive-shells)を確認する。 |
| Docker・MSBuild・Unity・Xcode が失敗する | 下記のツール別権限を確認する。ホストの共有状態が露出する場合がある。 |

**追加の許可は隔離を弱めます。** Docker デーモンへのアクセスはサンドボックスを迂回します。
Apple セキュリティサービスは Keychain 操作を可能にすることがあり、
Unity・Xcode の権限はホストの共有 IPC・キャッシュ・一時ファイルへのアクセスを許可する場合があります。
対象コード・入力・対象コードを変更できる先行ステップを信頼できる場合のみ有効化してください。

ホストからの完全な隔離ではありません。共有キャッシュ、ステップ隔離の外で動く組み込み処理、
許可したサービスにも信頼上の境界があります。
シークレットは許可先サービスへ流出し得ます。ログのマスキングはベストエフォートであり、安全性の境界ではありません。
機密データや追加の権限を使う前に、[セキュリティモデル全体（英語）](docs/reference.md#security-model-and-limitations)を確認してください。

## 詳細リファレンス

[リファレンスガイド（英語）](docs/reference.md)は README の利用者向け仕様の一部です。
従来の詳細な設定、互換性の規則、安全上の注意点を保持しています。
README 本体は日本語で読めますが、詳細ガイドは英語版を参照してください。

- [環境変数・シークレット・Java/Maven の既定値・出力・クリーンアップ](docs/reference.md#explicit-grants-and-inputs)
- [ユーザー設定](docs/reference.md#user-configuration)と[セットアップステップ](docs/reference.md#cpp-actions-only-setup-steps)
- [Apple セキュリティ](docs/reference.md#opt-in-apple-security-services)、[Xcode / Simulator](docs/reference.md#opt-in-xcode--simulator-compatibility)、[Xcode 一時ファイル](docs/reference.md#opt-in-xcode-temporary-files)
- [Docker CLI](docs/reference.md#opt-in-docker-cli-access)と[MSBuild IPC](docs/reference.md#opt-in-msbuild-worker-ipc)
- [Unity EditorPrefs](docs/reference.md#opt-in-unity-editorprefs-inheritance-macos)、[Unity ファイル / IPC](docs/reference.md#opt-in-unity-startup-files-and-ipc)、[Unity ネットワークホスト](docs/reference.md#opt-in-unity-network-hosts)

## ライセンス

[MIT](LICENSE)
