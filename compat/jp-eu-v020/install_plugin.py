#!/usr/bin/env python3
"""日本語版または欧州版のプラグインを安全に配置する補助プログラム。"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import sys
from datetime import datetime


def main():
    p = argparse.ArgumentParser(description="BBHost 日本語・欧州オンライン用プラグインの配置")
    p.add_argument("--edition", choices=("eu", "jp"), required=True, help="eu=欧州GOTY版、jp=日本版")
    p.add_argument("--plugins-dir", type=Path, required=True, help="BBHostが使用するプラグインディレクトリ")
    p.add_argument("--platform", choices=("linux", "windows"), default=("windows" if os.name == "nt" else "linux"), help="配置するプラットフォーム")
    p.add_argument("--dry-run", action="store_true", help="書き込みをせずに確認")
    args = p.parse_args()
    variant = "eu-goty" if args.edition == "eu" else "jp-old-hunters"
    ext = "dll" if args.platform == "windows" else "so"
    src = Path(__file__).resolve().parent / args.platform / variant / f"region_switch.{ext}"
    dst = args.plugins_dir.expanduser().resolve() / f"region_switch.{ext}"
    if not src.is_file(): p.error(f"プラグインがありません: {src}")
    if not dst.parent.is_dir(): p.error(f"プラグインの保存先がありません: {dst.parent}")
    if dst.is_symlink(): p.error("リンク先を上書きしません。手動で確認してください。")
    digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
    print("元ファイル:", src)
    print("保存先:", dst)
    print("SHA-256:", digest(src))
    if dst.is_file() and digest(src) == digest(dst):
        print("既に同じファイルが配置されています。変更なし。")
        return
    if args.dry_run:
        print("確認のみ完了。ファイルは変更していません。")
        return
    if dst.exists():
        stamp=datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        backup=dst.with_name(dst.name+".before-jp-eu-"+stamp+".bak")
        shutil.copy2(dst,backup)
        print("バックアップ:",backup)
    tmp=dst.with_name(dst.name+".install-tmp")
    if tmp.exists(): p.error("一時ファイルが既にあります。確認してください。")
    try:
        shutil.copy2(src,tmp)
        if digest(src) != digest(tmp): p.error("コピー後のハッシュが一致しません。")
        os.replace(tmp,dst)
    finally:
        if tmp.exists(): tmp.unlink()
    print("配置が完了しました。BBHost の設定・HTTP 修正・再ビルドも必要です。")

if __name__ == "__main__": main()
