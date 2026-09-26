import path from 'path';

import fs from 'fs-extra';

import Preferences from './preferences';

const pad = (v: number, p = 2) => v.toString().padStart(p, '0');

const getFormattedTime = (date = new Date()) =>
	`${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(
		date.getDate()
	)}-${pad(date.getHours())}-${pad(date.getMinutes())}-${pad(
		date.getSeconds()
	)}`;

export const getTimeElapsed = (startDate: Date, endDate = new Date()) => {
	let ms = endDate.getTime() - startDate.getTime();
	const m = Math.floor(ms / (1000 * 60));
	ms %= 1000 * 60;
	const s = Math.floor(ms / 1000);
	ms %= 1000;
	return `${m ? `${pad(m)}:` : ''}${pad(s)}.${pad(ms, 3)}`;
};

type MessageType = 'error' | 'warning' | 'info';
type LoggerMessage = {
	message: string;
	type: MessageType;
	time: string;
	obj?: unknown;
};

abstract class Logger {
	static #history: LoggerMessage[] = [];

	static #messageToString = ({ message, type, time, obj }: LoggerMessage) => {
		const objString = obj
			? `\n${obj instanceof Error ? obj.stack : JSON.stringify(obj, null, 2)}`
			: '';

		return `[${type}][${time}]: ${message}${objString}`;
	};

	static async log(message: string, type?: 'error' | 'warning', obj?: unknown) {
		const newMessage: LoggerMessage = {
			message,
			type: type ?? 'info',
			time: new Date().toLocaleTimeString(),
			obj
		};
		console.log(this.#messageToString(newMessage));
		this.#history.push(newMessage);
	}

	static async saveDiagnostics(diagnostics: unknown) {
		try {
			await fs.outputJSON(
				path.join(Preferences.userDataDir, 'runtime.json'),
				{ time: new Date().toISOString(), ...(diagnostics as object) },
				{ spaces: 2 }
			);
		} catch (e) {
			void this.log('Failed to write runtime diagnostics', 'warning', e);
		}
	}

	static async saveLog() {
		const files = await fs.readdir(Preferences.userDataDir);

		// Delete old logs and keep up to 4
		for (const file of files.filter(f => f.startsWith('log-')).slice(0, -4)) {
			await fs.remove(path.join(Preferences.userDataDir, file));
		}

		// Save current log
		await fs.writeFile(
			path.join(Preferences.userDataDir, `log-${getFormattedTime()}.txt`),
			this.#history.map(this.#messageToString).join('\n')
		);
	}
}
export default Logger;
