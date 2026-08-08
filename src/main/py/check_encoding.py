from chardet.universaldetector import UniversalDetector
import chardet
import os
import sys
import subprocess
import site

#########################################################
# 定数
#########################################################
patternUTF8_BOM   = (
	"utf-8-sig",
	"ascii"
)
patternUTF8_NoBOM = (
	"utf-8"
)
patternUTF16 = (
	"utf-16"
)
expect_encoding = {
	".cpp" : patternUTF8_BOM,
	".h"   : patternUTF8_BOM,
	".rc"  : patternUTF16,
	".rc2" : patternUTF16
}

# チェック対象の拡張子リスト
extensions = expect_encoding.keys()

# 指定したファイルの文字コードを返す
def check_encoding(file_path):
	detector = UniversalDetector()
	with open(file_path, mode='rb') as f:
		for binary in f:
			detector.feed(binary)
			if detector.done:
				break
	detector.close()
	return detector.result['encoding']

# チェック対象の拡張子か判断する
def check_extension(file_name):
	_, ext = os.path.splitext(file_name)
	return (ext in extensions)

# Git command output is decoded here so the unit tests can use either bytes or
# text mocks while CI still receives the exact subprocess argument arrays.
def _decode_output(output):
	if isinstance(output, bytes):
		return output.decode()
	return output


def _is_full_scan_base(base_sha):
	return not base_sha or base_sha == ('0' * 40)


def _validate_base_sha(base_sha):
	"""Validate that a non-fallback base value resolves to a commit.

	The SHA comes from the workflow event, but it is still passed as one
	argument and validated before it is used in the diff command.
	"""
	subprocess.check_output([
		'git', 'rev-parse', '--verify', '--end-of-options',
		base_sha + '^{commit}'
	], stderr=subprocess.STDOUT)


# ベースとの差分をチェック
def get_diff_files(base_sha):
	_validate_base_sha(base_sha)
	output = subprocess.check_output([
		'git', 'diff', '--name-only', '--diff-filter=d', '-z',
		base_sha, 'HEAD', '--'
	], stderr=subprocess.STDOUT)
	for file_name in _decode_output(output).split('\0'):
		if not file_name:
			continue
		if check_extension(file_name):
			yield file_name
		else:
			print ("skip " + file_name)


def get_tracked_files():
	"""Return tracked target files without walking build/venv directories."""
	output = subprocess.check_output([
		'git', 'ls-files', '-z', '--'
	], stderr=subprocess.STDOUT)
	for file_name in _decode_output(output).split('\0'):
		if file_name and check_extension(file_name):
			yield file_name
		elif file_name:
			print ("skip " + file_name)


def get_ci_files(base_sha):
	"""Select a diff scan or safe tracked-file fallback for CI."""
	if _is_full_scan_base(base_sha):
		print ("base SHA is empty or all-zero; checking tracked files")
		return get_tracked_files()
	return get_diff_files(base_sha)

# デバッグ用
# すべてのファイルを対象にチェック対象の拡張子のファイルの文字コードを調べてチェックする
def check_all():
	for rootdir, dirs, files in os.walk('.'):
		for file_name in files:
			if check_extension(file_name):
				full = os.path.join(rootdir, file_name)
				yield full

# 指定したファイルの文字コードが期待通りか確認する
def check_encoding_result(file_name, encoding):
	_, ext = os.path.splitext(file_name)
	encoding = encoding.lower()
	if encoding in expect_encoding.get(ext, ()):
		return True
	return False

# 指定されたファイルリストに対して文字コードが適切かチェックする
# (条件に満たないファイル数を返す。)
def process_files(files):
	# 条件に満たないファイル数
	count = 0

	for file_name in files:
		print ("checking " + file_name)
		encoding = check_encoding(file_name)
		if not check_encoding_result(file_name, encoding):
			print ("NG", encoding, file_name)
			count = count + 1
		else:
			print ("OK", encoding, file_name)
	return count

if __name__ == '__main__':
	user_scripts = os.path.join(site.USER_BASE, "Scripts")
	sys.path.append(user_scripts)
	print ("adding " + user_scripts + " to PATH")

	count = 0
	if len(sys.argv) > 1 and sys.argv[1] == "all":
		count = process_files(check_all())
	else:
		base_sha = os.environ.get('CHECK_ENCODING_BASE_SHA', '').strip()
		count = process_files(get_ci_files(base_sha))

	if count > 0:
		print ("return 1")
		sys.exit(1)
	else:
		print ("return 0")
		sys.exit(0)
