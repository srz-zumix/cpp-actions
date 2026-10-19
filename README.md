# cpp-actions

macOS 上で GitHub Actions ワークフローを実行する C++20 CLI です。
通常のステップは Seatbelt サンドボックス内で実行します。

## ビルド

CMake 3.24 以上、Ninja、C++20 コンパイラーが必要です。

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## 実行

```sh
build/debug/src/cpp-actions validate
build/debug/src/cpp-actions run --dry-run
build/debug/src/cpp-actions run --event workflow_dispatch --input greeting=hello
```

`run --help` に全オプションを表示します。`--workflow` と `--job` で対象を選択し、
`--keep-workdir` で実行後のワークスペースを残せます。
結果とログは表示された実行ディレクトリに保存されます。

## アクション（Phase 5）

- `uses: ./path` はチェックアウト済みワークスペース内の `action.yml`／`action.yaml` を読み込みます。
- `owner/repo[/path]@ref` は GitHub から取得し、ref をコミット SHA に解決して
  `--cache-dir` 配下の `actions` にキャッシュします。
- JavaScript の `node20`／`node24`、`INPUT_*`、pre／main／post、
  `pre-if`／`post-if`、`GITHUB_STATE` に対応します。
  対応するホスト Node を優先し、必要なら `actions/node-versions` の GitHub Releases から取得します。
  取得できない場合はホスト Node に警告付きでフォールバックします。
  `--node /absolute/path/to/node` で使用する実行ファイルを指定できます。
- composite アクションは inputs／outputs、`github.action_path`、入れ子のステップに対応します。
  入れ子は最大 10 段です。
- `actions/checkout` は自リポジトリの作業ツリー（未コミット変更を含む）を配置します。
- `actions/cache`（`/restore`、`/save` を含む）は `--cache-dir` 配下の `cache` を使います。
  GitHub 上のキャッシュとは共有されません。
- `actions/upload-artifact`／`actions/download-artifact` は同じワークフロー実行内で
  ジョブ間のファイルを受け渡します。保存先は実行ディレクトリ配下の `artifacts` です。
  他の実行からの取得や GitHub の保持期限・圧縮・ストレージ API は再現しません。
- setup 系を含むその他の JavaScript／composite アクションはそのまま実行します。
  `RUNNER_TOOL_CACHE` は `--cache-dir` 配下の `toolcache` を指し、実行間で共有します。
- Docker コンテナアクションには明確な未対応エラーを返します。

## セキュリティと制限

既定の実行対象は macOS ランナーです。GitHub ホステッドランナーのツール構成は再現しません。
Linux ではビルドとサンドボックス以外のテストを実行できます。
開発時に Linux 上で実行する場合は、信頼できるワークフローに限って
`--host-label ubuntu-latest --no-sandbox` を明示します。
`--no-sandbox` はファイル・ネットワークの隔離を無効にする危険なデバッグ用オプションです。

ステップのネットワークは許可制プロキシ経由です。GitHub 関連以外の取得先を必要とする
setup 系アクションなどは `--allow-host` でホストを追加してください。
ホスト環境は既定で引き継がず、必要な変数は `--env` で明示します。
シークレットは `--secrets-file` または `--secret` で渡します。
GitHub 認証も `--github-token` による明示指定のみです。コマンドライン引数は
シェル履歴やプロセス一覧に残り得るため注意してください。
アクション取得と組み込みエミュレーションはホスト側で行います。
許可したアクションや共有ツールキャッシュは信頼できるものだけを使ってください。