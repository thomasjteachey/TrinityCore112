import { z } from 'zod';

import {
        DEFAULT_AZEROTHCORE_REALMLIST,
        DEFAULT_LAUNCHER_UPDATE_URL,
        DEFAULT_REALM_ID,
        DEFAULT_REALMLIST,
        REALM_IDS
} from './constants';

/**
 * Zod type wrappers for use with form inputs
 */
const protocolRegex = /^[a-zA-Z][a-zA-Z\d+\-.]*:\/\//;

const normalizeLauncherUrl = (value: string) => {
        const trimmed = value.trim();
        const withProtocol = protocolRegex.test(trimmed) ? trimmed : `http://${trimmed}`;
        return withProtocol.endsWith('/') ? withProtocol : `${withProtocol}/`;
};

const f = {
        boolean: (defaultValue?: boolean) =>
                z.boolean().nullish().default(!!defaultValue),
        number: (defaultValue?: number, val?: (v: z.ZodNumber) => z.ZodNumber) =>
		z.preprocess(
			v =>
				v === '' || v === undefined
					? defaultValue ?? null
					: typeof v === 'string'
					? Number(v)
					: v,
			(val?.(z.number()) ?? z.number()).nullish()
		)
};

export const PreferencesSchema = z.object({
        isPortable: z.boolean().optional(),
        isDev: f.boolean(),
        clientDir: z.string().optional(),
        // Whether this client has already received the launcher's one-time
        // Config.wtf defaults. WoW omits any cvar equal to its default when it
        // rewrites the file, so a launcher that re-seeds every launch cannot tell
        // "absent because the player chose the default" from "absent because new
        // install", and keeps resurrecting settings the player turned off.
        clientInitialized: f.boolean(),
        reopenLauncher: f.boolean(),
        cleanWdb: f.boolean(true),
        // Rewrites the conditional at Wow.exe+0x0e94 so maximized-windowed
        // renders as true borderless fullscreen. Stock is 0x74 (JZ); the patch
        // makes it 0xEB (JMP). On by default because that is what the client has
        // always shipped with here, but it has to be switchable: under Wine,
        // Proton and WoWSilicon the borderless path makes the client perform a
        // real display-mode change, which blanks other monitors and lets
        // DirectInput grab the mouse with no way to tab out.
        borderlessFullscreen: f.boolean(true),
        rememberPosition: f.boolean(),
        launcherUpdateUrl: z
                .string()
                .trim()
                .min(1)
                .transform(normalizeLauncherUrl)
                .default(DEFAULT_LAUNCHER_UPDATE_URL),
        realmList: z.string().trim().min(1).default(DEFAULT_REALMLIST),
        azerothcoreRealmList: z
                .string()
                .trim()
                .min(1)
                .default(DEFAULT_AZEROTHCORE_REALMLIST),
        windowPosition: z
                .object({
                        x: z.number(),
                        y: z.number(),
                        width: z.number(),
                        height: z.number()
                })
                .nullish(),
        selectedRealm: z.enum(REALM_IDS).default(DEFAULT_REALM_ID),
        optionalPatches: z.array(z.string()).default([])
});
export type PreferencesSchema = z.infer<typeof PreferencesSchema>;
