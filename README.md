# cpp-actions

GitHub Actions のワークフローをローカルで実行する C++20 CLI です。
現在は Phase 3（`run:` ステップの実行）までを対象としています。

> **警告:** 現在の `run` はサンドボックスなしで実行します。ステップはホストの
> ファイルとネットワークにアクセスできます。信頼できるワークフローだけを実行してください。
> 作業ツリーのコピーや専用 HOME はセキュリティ境界ではありません。
> Seatbelt と許可制ネットワークは Phase 4 の対象です。

## ビルドとテスト

CMake 3.24 以上、Ninja、C++20 コンパイラ、git が必要です。
依存ライブラリは CMake が取得します。

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

通常の実行対象は macOS です。Linux ではビルドとユニットテストが可能です。
CI では macOS / Ubuntu のビルドと、macOS の ASan / UBSan を実行します。

## 使い方

```sh
build/debug/src/cpp-actions validate -C /path/to/repository
build/debug/src/cpp-actions list -C /path/to/repository
build/debug/src/cpp-actions graph -C /path/to/repository --format dot

# 実行せずにイベントの一致と依存グラフを確認
build/debug/src/cpp-actions run -C /path/to/repository --dry-run

# push イベント（既定）で実行。ワークフローは名前・ファイル名・パスで選択可能
build/debug/src/cpp-actions run -C /path/to/repository -w ci.yml --parallel 2

# workflow_dispatch の inputs、明示的なシークレットと変数
build/debug/src/cpp-actions run -C /path/to/repository -e workflow_dispatch \
  --input configuration=debug --secrets-file /private/secrets.env \
  --vars-file /private/vars.env --var MODE=local
```

`--event-path` でイベント JSON を指定できます。`--ref`、`--actor`、
`--base-ref`、`--head-ref` などでもイベントを調整できます。詳細は `run --help` を参照してください。
`workflow_dispatch` の inputs はイベント JSON からも読み込み、同名の `--input` が優先されます。
一致するワークフローがない場合や `--dry-run` では作業領域を作りません。

### Phase 3 の実行

- `needs` の依存関係、ジョブ／ステップの `if`、matrix、
  `fail-fast`、`max-parallel`、`continue-on-error`、タイムアウトに対応します。
  Ctrl-C でキャンセルし、再度のキャンセルで強制終了します。子プロセスもプロセスグループ単位で停止します。
- `macos-*` ジョブは macOS ホストで実行します。Ubuntu / Windows などのジョブは
  理由を表示してスキップします。デバッグ用の `--host-label LABEL` は指定したラベルを
  現在のホストで実行しますが、ランナーイメージを再現するものではありません。
- `actions/checkout` による自リポジトリの取得は、ローカル作業ツリーのスナップショットで
  代替します。未コミットの変更、削除、ignore されていない未追跡ファイルを含みます。
  git の履歴もコピーし、変更がある場合はスナップショット内だけにコミットします。
  元のリポジトリは変更しません。checkout までジョブのワークスペースは空です。
- bash、sh、python、pwsh、`{0}` を含む独自シェル指定と、
  `defaults.run`、`working-directory`、階層ごとの env に対応します。
  シェルやツールはホストにインストールされたものを使います。
- `github`、`env`、`vars`、`secrets`、`job`、`steps`、`runner`、`strategy`、
  `matrix`、`needs`、`inputs` のコンテキスト、ステップ／ジョブ outputs と
  ワークスペース基準の `hashFiles` を使用できます。
- `GITHUB_ENV`、`GITHUB_OUTPUT`、`GITHUB_PATH`、`GITHUB_STATE`、
  `GITHUB_STEP_SUMMARY` のファイルコマンド、group / endgroup、
  error / warning / notice / debug、add-mask、stop-commands を処理します。
  `GITHUB_STATE` はアクションの state 用で、通常の後続 `run:` の env にはなりません。

### 環境変数とシークレット

ホストの環境変数を一括では引き継ぎません。専用 HOME / TMPDIR、PATH などの最小環境を
構築します。追加する場合は `--env-pass NAME` または `--env-pass NAME=VALUE` を指定します。
秘密値は `--env-pass` ではなく `--secrets-file` / `--secret NAME=VALUE` で渡してください。
コマンドライン引数はシェル履歴やプロセス一覧に残るため、シークレットにはファイルを推奨します。

ファイルは dotenv 形式（`NAME=VALUE`、引用符、コメント、`export`）です。
同名の値は CLI 指定がファイルより優先されます。シークレットは明示的に渡したものだけを
使用し、ログではマスクします。`GITHUB_TOKEN` も自動取得しません。
`--secrets-file` または `--secret` で指定した `GITHUB_TOKEN` は
`secrets.GITHUB_TOKEN` と `github.token` で参照できます。
シークレットは env に自動設定されないため、ステップの `env:` で必要な値を指定してください。
ワークフローが任意のファイルへ書いた秘密値までは除去できません。

### 結果と作業領域

ジョブ／ステップごとにログを表示し、最後に結果と step summary を表示します。
`--debug` で debug コマンドを表示し、`--no-color` または `NO_COLOR` で色を無効にできます。
終了コードは成功／スキップが `0`、実行失敗が `1`、入力エラーが `2`、キャンセルが `130` です。

実行ごとのディレクトリに `logs/`、`report.json`、`summary.md`（summary がある場合）を保存します。
既定では共有キャッシュ内の `runs/` に作成し、作業用ワークスペースとスナップショットは
終了時に削除します。`--keep-workdir` で調査用に保持できます。保持したファイルには
env やスクリプト中の秘密値が残り得るため、取り扱いに注意してください。
`--run-dir` には未作成のディレクトリを指定し、`--cache-dir` で共有キャッシュを変更できます。

## 制限

Docker コンテナ、GitHub ホステッドランナーのインストール済みツールの再現、
GitHub のキャッシュ／アーティファクト API や OIDC サービスは対象外です。
checkout 以外のアクション、再利用ワークフロー、高度な設定の対応は後続フェーズの対象で、
Phase 3 の互換性保証には含みません。ローカル実行は GitHub 上での実行結果を保証しません。