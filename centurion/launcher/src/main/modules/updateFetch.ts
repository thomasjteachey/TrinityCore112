import https from 'node:https';
import net from 'node:net';

import fetch, { type RequestInit } from 'node-fetch';

import { DEFAULT_LAUNCHER_UPDATE_URL } from '~common/constants';

import Logger from './logger';

const canonicalUpdateHostname = new URL(DEFAULT_LAUNCHER_UPDATE_URL).hostname;
const directIpAgent = new https.Agent({
	// A LAN client can connect straight to the update server's IP without DNS,
	// while TLS still authenticates the hostname named by its public certificate.
	// Do not replace this with rejectUnauthorized: false.
	servername: canonicalUpdateHostname
});

/**
 * Extra attempts after the first one. A verify pass fires one request per
 * FileMap entry back to back, so a single transient failure used to abort the
 * whole pass and surface as "Failed to download <whichever patch was unlucky>".
 * Players saw the name rotate between patches on every retry, which reads like
 * missing files on the server rather than a dropped connection.
 */
const RETRY_ATTEMPTS = 3;
const RETRY_BASE_DELAY = 400;

/**
 * Worth another attempt: the request never produced a usable response, or the
 * server asked us to come back later. A 404 is NOT retried - the file really is
 * absent and hammering it just delays the real error.
 */
const isRetriableStatus = (status: number) =>
	status === 408 || status === 425 || status === 429 || status >= 500;

const sleep = (ms: number) =>
	new Promise<void>(resolve => {
		setTimeout(resolve, ms);
	});

export type UpdateFetchInit = RequestInit & {
	/**
	 * Extra attempts on a transient failure. Pass 0 to disable.
	 */
	retries?: number;
};

/**
 * Fetch an update resource. HTTPS URLs that use a literal IP connect to that IP
 * but use the canonical update hostname for SNI and certificate verification.
 *
 * Transient failures are retried with backoff. Only failures that happen before
 * the body is read are retried, so this is safe for the streaming patch
 * downloads too - a connection dropped mid-body surfaces on the stream instead,
 * where resumableFetch resumes it from the byte it reached.
 *
 * `timeout` is left to the caller because node-fetch v2 applies it as an
 * absolute deadline rather than an idle timeout: setting one here would abort
 * any patch download that legitimately takes longer than it.
 */
const updateFetch = async (url: string, init?: UpdateFetchInit) => {
	const parsedUrl = new URL(url);
	const agent =
		parsedUrl.protocol === 'https:' && net.isIP(parsedUrl.hostname)
			? directIpAgent
			: init?.agent;

	const { retries = RETRY_ATTEMPTS, ...requestInit } = init ?? {};
	let lastError: unknown;

	for (let attempt = 0; attempt <= retries; attempt += 1) {
		if (attempt !== 0) {
			// Exponential backoff with jitter, so a client that trips a rate
			// limiter does not march back in lockstep with every other client.
			const backoff = RETRY_BASE_DELAY * 2 ** (attempt - 1);
			await sleep(backoff + Math.floor(Math.random() * RETRY_BASE_DELAY));
		}

		try {
			const response = await fetch(url, { ...requestInit, agent });
			const usable = !isRetriableStatus(response.status);

			// Out of attempts: hand back even a retriable response so the caller
			// reports the real status instead of a bare connection error.
			if (usable || attempt === retries) {
				if (usable && attempt !== 0) {
					void Logger.log(
						`Recovered ${url} after ${attempt + 1} attempts.`,
						'warning'
					);
				}
				return response;
			}

			lastError = new Error(
				`${response.status} ${response.statusText} from ${url}`
			);
			// Drain the discarded body so the socket is released rather than left
			// hanging until the pool times it out.
			response.body?.resume();
		} catch (e) {
			lastError = e;
		}
	}

	throw lastError instanceof Error
		? lastError
		: new Error(`Failed to fetch ${url}`);
};

export default updateFetch;
