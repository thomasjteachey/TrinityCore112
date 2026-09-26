import fs from 'fs-extra';

import Logger from './logger';
import updateFetch from './updateFetch';

type ProgressOptions = {
	throttle?: number;
	/**
	 * When true, ignore any existing complete download and delete
	 * any partial file before starting. Used for "self-heal" retries.
	 */
	force?: boolean;
};

export type FetchProgress = {
	total: number;
	done: number;
	initialPartial: number;
	startedAt: number;
};

const throttle = (ms: number, fn: () => void) => {
	if (ms === 0) return fn;
	let last = 0;
	return () => {
		const now = Date.now();
		if (now - last > ms) {
			last = now;
			fn();
		}
	};
};

const resumableFetch = async (
	url: string,
	downloadPath: string,
	callback?: (args: FetchProgress) => void,
	options: ProgressOptions = {}
) => {
	const { force, throttle: throttleMs } = options;
	const partialPath = `${downloadPath}.partial`;

	if (!force && (await fs.exists(downloadPath))) {
		Logger.log(`File "${downloadPath}" already exists. Skipping download.`);
		return;
	}

	if (force) {
		try {
			if (await fs.exists(downloadPath)) {
				Logger.log(`Force re-download requested. Removing "${downloadPath}".`);
				await fs.remove(downloadPath);
			}
			if (await fs.exists(partialPath)) {
				Logger.log(`Force re-download requested. Removing "${partialPath}".`);
				await fs.remove(partialPath);
			}
		} catch (e) {
			Logger.log(
				`Failed to remove existing download for "${downloadPath}"`,
				'error',
				e
			);
		}
	}

	let initialPartial = (await fs.exists(partialPath))
		? (await fs.stat(partialPath)).size
		: 0;
	let done = 0;

	if (initialPartial) {
		Logger.log(
			`Resuming download of "${downloadPath}" from ${initialPartial} bytes`
		);
	} else {
		Logger.log(`Downloading "${downloadPath}"`);
	}

	let response = await updateFetch(
		url,
		initialPartial
			? { headers: { Range: `bytes=${initialPartial}-` } }
			: undefined
	);

	const contentRangeStart = Number(
		response.headers.get('content-range')?.match(/^bytes (\d+)-/i)?.[1]
	);
	const invalidResume =
		initialPartial > 0 &&
		(response.status === 200 ||
			response.status === 416 ||
			(response.status === 206 && contentRangeStart !== initialPartial));

	if (invalidResume) {
		Logger.log(
			`Discarding stale partial download "${partialPath}" and restarting.`
		);
		(response.body as { destroy?: () => void } | null)?.destroy?.();
		await fs.remove(partialPath);
		initialPartial = 0;
		response = await updateFetch(url);
	}

        if (!response.ok || !response.body) {
                throw new Error(
                        `Failed to download "${url}" (${response.status} ${response.statusText})`
                );
        }

        const total = Number(response.headers.get('content-length'));
        const startedAt = Date.now();

        const throttled = throttle(throttleMs ?? 0, () => {
                callback?.({ total, done, initialPartial, startedAt });
        });

        await new Promise<void>((resolve, reject) => {
                const writeStream = fs.createWriteStream(partialPath, {
                        flags: initialPartial ? 'a' : 'w'
                });

                response.body?.on('data', chunk => {
                        done += chunk.length;
                        throttled();
                });

                response.body?.on('end', () => {
                        callback?.({ total, done, initialPartial, startedAt });
                });

                response.body?.on('error', error => {
                        writeStream.destroy(error);
                        reject(error);
                });

                writeStream.on('error', reject);

                writeStream.on('finish', () => {
                        resolve();
                });

                response.body?.pipe(writeStream);
        });

        await fs.rename(partialPath, downloadPath);
        Logger.log(`Downloaded "${downloadPath}"`);
};

export default resumableFetch;
