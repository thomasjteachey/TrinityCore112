export const isNotUndef = <T>(obj: T): obj is Exclude<T, undefined> =>
	obj !== undefined;

export const formatFileSize = (bytes: number) => {
	const units = ['B', 'KB', 'MB', 'GB', 'TB'];
	let size = bytes;
	let unitIndex = 0;

	while (size >= 1024 && unitIndex < units.length - 1) {
		size /= 1024;
		unitIndex++;
	}

	return `${size.toFixed(2)} ${units[unitIndex]}`;
};

export const formatDuration = (remaining: number) => {
	const hours = Math.floor(remaining / 3600);
	const minutes = Math.floor((remaining % 3600) / 60);
	const seconds = Math.floor(remaining % 60);

	return `${hours ? `${hours}h ` : ''}${
		minutes ? `${minutes}m ` : ''
	}${seconds}s`;
};

export const focusBlur = <T extends { currentTarget: { blur: () => void } }>(
	callback?: ((e: T) => void) | true
) =>
	callback
		? (e: T) => {
				typeof callback === 'function' && callback(e);
				e?.currentTarget?.blur();
		  }
		: undefined;

export const omit = <T extends object, const K extends keyof T>(
	obj: T,
	keys: K[]
): Omit<T, K> => {
	const result = { ...obj };
	keys.forEach(key => {
		delete result[key];
	});
	return result;
};
