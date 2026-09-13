import os
import zipfile
import shutil

def package_decky():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)
    plugin_dir = os.path.join(project_dir, "decky-plugin")
    output_zip = os.path.join(project_dir, "SkyFrame.zip")

    # 1. Verify dist/ directory exists with compiled index.js
    dist_dir = os.path.join(plugin_dir, "dist")
    index_js = os.path.join(dist_dir, "index.js")
    if not os.path.isfile(index_js):
        raise RuntimeError("dist/index.js is missing! Run 'npm run build' inside decky-plugin first.")

    # 2. Ensure bin/ directory exists
    bin_dir = os.path.join(plugin_dir, "bin")
    os.makedirs(bin_dir, exist_ok=True)

    # 3. Create zip archive with "SkyFrame/" root directory
    print(f"Creating Decky Plugin archive: {output_zip}...")
    if os.path.isfile(output_zip):
        os.remove(output_zip)

    with zipfile.ZipFile(output_zip, "w", zipfile.ZIP_DEFLATED) as zipf:
        # Include plugin files
        for root, dirs, files in os.walk(plugin_dir):
            # Exclude node_modules, src (TypeScript raw), etc.
            if "node_modules" in root or "src" in root:
                continue
            for file in files:
                file_path = os.path.join(root, file)
                rel_path = os.path.relpath(file_path, plugin_dir)
                archive_name = os.path.join("SkyFrame", rel_path).replace("\\", "/")
                zipf.write(file_path, archive_name)

    print(f"[SUCCESS] Successfully created {output_zip} ({os.path.getsize(output_zip)} bytes)")

if __name__ == "__main__":
    package_decky()
