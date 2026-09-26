import path from 'node:path';

import fs from 'fs-extra';

type PatchCaches = {
	versions: Record<string, string>;
	files: Record<string, string[]>;
};

type RenamablePatch = {
	extractPath: string;
	migrateFrom?: string;
	extractAs?: Record<string, string>;
};

/**
 * Moves a patch that changed FileMap key and local archive name (the art base,
 * patch-Y.MPQ -> patch-X.MPQ) over in place: the files are renamed on disk and
 * the recorded version is carried to the new key, so a client that is already
 * up to date does not download the same archive again.
 *
 * Does nothing unless the cache still records the old key, nothing is recorded
 * under the new one, and every file to rename exists with no file already at
 * its new name. Returns the new file names, or an empty list when skipped.
 */
export const migrateRenamedPatch = async (
	clientDir: string,
	name: string,
	meta: RenamablePatch,
	caches: PatchCaches
) => {
	const from = meta.migrateFrom;
	if (!from || !meta.extractAs) return [];
	if (caches.versions[name] || !caches.versions[from]) return [];

	const recorded = (caches.files[from] ?? []).map(file => file.toLowerCase());
	const renames = Object.entries(meta.extractAs).filter(([oldFile]) =>
		recorded.includes(oldFile.toLowerCase())
	);
	if (renames.length === 0) return [];

	const dir = path.join(clientDir, meta.extractPath);
	for (const [oldFile, newFile] of renames) {
		if (!(await fs.pathExists(path.join(dir, oldFile)))) return [];
		if (await fs.pathExists(path.join(dir, newFile))) return [];
	}

	for (const [oldFile, newFile] of renames) {
		await fs.rename(path.join(dir, oldFile), path.join(dir, newFile));
	}

	const renamed = renames.map(([, newFile]) => newFile);
	caches.versions[name] = caches.versions[from];
	caches.files[name] = renamed;
	delete caches.versions[from];
	delete caches.files[from];
	return renamed;
};

/**
 * Deletes the named files from a patch's extract path, matching names without
 * regard to case (Proton clients sit on case-sensitive filesystems). Returns
 * the names that were actually removed.
 */
export const removePatchFiles = async (
	clientDir: string,
	extractPath: string,
	files: string[]
) => {
	const dir = path.join(clientDir, extractPath);
	const wanted = new Set(files.map(file => file.toLowerCase()));
	const entries = await fs.readdir(dir).catch(() => [] as string[]);

	const removed: string[] = [];
	for (const entry of entries) {
		if (!wanted.has(entry.toLowerCase())) continue;
		await fs.remove(path.join(dir, entry));
		removed.push(entry);
	}
	return removed;
};
