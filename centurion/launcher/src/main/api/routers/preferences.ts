import { z } from 'zod';

import { PreferencesSchema } from '~common/schemas';
import Preferences from '~main/modules/preferences';
import Updater from '~main/modules/updater';

const areArraysEqual = (a?: string[], b?: string[]) => {
        if (a === b) return true;

        const first = [...(a ?? [])].sort();
        const second = [...(b ?? [])].sort();

        if (first.length !== second.length) {
                return false;
        }

        return first.every((value, index) => value === second[index]);
};

import { createTRPCRouter, publicProcedure } from '../trpc';

export const preferencesRouter = createTRPCRouter({
	get: publicProcedure.output(PreferencesSchema).query(() => Preferences.data),
        set: publicProcedure
                .input(PreferencesSchema.partial())
                .mutation(async ({ input }) => {
                        const previousPreferences = Preferences.data;

                        if (
                                input.clientDir &&
                                !(await Preferences.isValidClientDir(input.clientDir))
                        ) {
                                throw new Error('Invalid client directory. WoW.exe not found.');
                        }
                        const previousUrl = previousPreferences.launcherUpdateUrl;
                        Preferences.data = input;

                        const currentPreferences = Preferences.data;

                        if (
                                input.launcherUpdateUrl !== undefined &&
                                currentPreferences.launcherUpdateUrl !== previousUrl
                        ) {
                                void Updater.verify();
                        }

                        const realmChanged =
                                input.selectedRealm !== undefined &&
                                currentPreferences.selectedRealm !== previousPreferences.selectedRealm;
                        const optionalPatchesChanged =
                                input.optionalPatches !== undefined &&
                                !areArraysEqual(
                                        previousPreferences.optionalPatches,
                                        currentPreferences.optionalPatches
                                );

                        if (realmChanged || optionalPatchesChanged) {
                                Updater.invalidate();
                        }

                        return currentPreferences;
                }),
	isValidClientDir: publicProcedure
		.input(z.string().optional())
		.query(({ input }) => Preferences.isValidClientDir(input))
});
