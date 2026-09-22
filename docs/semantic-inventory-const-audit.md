# インデント付きconst宣言の誤検出（Issue #309）

`inventory semantic --strict`が、`const WORD languageId;`を公開可変フィールドとして検出した。
先頭の`\s*`がバックトラックすると、除外キーワードの否定先読みが空白の手前で成功してしまう。
同じ形のraw pointer member判定にも影響する。宣言の前の空白も含めて先頭から除外を判定し、
既存のconst/static/constexpr除外をインデントの有無にかかわらず適用する。

| Verify | Expect | 実績 |
|---|---|---|
| 新しい回帰テストを修正前のscannerで実行 | 空白・タブで誤検出を再現 | インデントなしは成功、空白1・空白4・タブで失敗 |
| `python -m unittest tools.build.tests.test_semantic_inventory -v` | 実際の可変フィールドとポインタ検出を保持し、既存のratchet検査も成功 | 18/18成功 |
| `lint checkout-invariance` | LF/CRLFでscanner hashと投影が一致 | 成功 |
| `generate --check` | 古い投影がない | 成功 |

scannerのハッシュはbaselineの契約に含まれるため、この修正にはcleanな確定コミットからの
正式な`--accept-current`操作と履歴レコードが必要。baselineの手編集やCIでの自動受理は行わない。
前後のrule/file差分と受理理由は`tools/build/baselines/editor-core-semantic-history/`に記録する。

調査範囲はscannerの公開フィールド判定、回帰テスト、基準受理契約に限った。
範囲が広がった理由は、実装修正に伴ってscannerバージョンが変わると旧基準との比較を拒否する
既存のfail-closed契約にある。製品の状態所有者・公開API・依存関係は変わらない。
次回はこの回帰テストと受理レコードから、インデントによる誤検出と実際の負債増加を区別できる。
