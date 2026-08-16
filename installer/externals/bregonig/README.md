# 正規表現ライブラリ bregonig.dll

## Website

- http://k-takata.o.oo7.jp/mysoft/bregonig.html

## ソースコード

- http://github.com/k-takata/bregonig

製品がリンク・配布する `bregonig.dll` は、この ZIP ではなく
vcpkg が `externals/bregonig` からビルドした成果物である。

## ライセンス

インストーラと ZIP は `externals/bregonig` の次のファイルを同梱する。

- `bsd_license.txt`
- `perl_license.txt`
- `perl_license_jp.txt`

## 一時的な互換オラクル

- `bron420.zip` は上流配布バイナリの凍結コピーである。
- ビルド、テスト、インストーラ、ZIP、製品実行経路はここから DLL を展開してはならない。
- SHA-256・出所・ライセンスは `ORACLE.json` に記録する。
- 旧 DLL との差分 golden が [#185](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/185) で揃ったら削除する。
- 恒久的な reference artifact にはしない。
