import { useEffect, useRef, useState } from 'react';
import cls from 'classnames';
import { ChevronDown } from 'lucide-react';

import {
	DEFAULT_REALM_ID,
	isRetiredRealm,
	PUBLIC_REALM_IDS,
	REALMS,
	type RealmId
} from '~common/constants';
import { type UpdaterStatus } from '~main/types';
import { api } from '~renderer/utils/api';

const RealmSwitch = () => {
	const { data: pref } = api.preferences.get.useQuery();
	const setPref = api.preferences.set.useMutation();

	const invalidate = api.updater.invalidate.useMutation();
	const verify = api.updater.verify.useMutation();

	const [updaterState, setUpdaterState] =
		useState<UpdaterStatus['state']>('needsValidation');
	api.updater.observe.useSubscription(undefined, {
		onData: data => setUpdaterState(data.state)
	});

	const [open, setOpen] = useState(false);
	const rootRef = useRef<HTMLDivElement>(null);

	const isUpdaterBusy =
		updaterState === 'verifying' || updaterState === 'updating';
	const isMutating =
		setPref.isLoading || invalidate.isLoading || verify.isLoading;
	const isLoading = isUpdaterBusy || isMutating;

	const availableRealmEntries: [RealmId, (typeof REALMS)[RealmId]][] =
		pref?.isDev
			? (Object.entries(REALMS) as [RealmId, (typeof REALMS)[RealmId]][]).filter(
					([id]) => !isRetiredRealm(id)
				)
			: PUBLIC_REALM_IDS.map(id => [id, REALMS[id]]);

	const selectedRealm =
		pref?.selectedRealm &&
		availableRealmEntries.some(([id]) => id === pref.selectedRealm)
			? pref.selectedRealm
			: DEFAULT_REALM_ID;

	const onSelect = async (realm: RealmId) => {
		setOpen(false);
		if (isLoading) return;
		if (selectedRealm === realm) return;
		await setPref.mutateAsync({ selectedRealm: realm });
		await invalidate.mutateAsync();
		await verify.mutateAsync();
	};

	// Close on an outside press or Escape. Bound on pointerdown so it runs in the
	// same phase the options activate in, rather than a phase later.
	useEffect(() => {
		if (!open) return;

		const handlePointerDown = (event: PointerEvent) => {
			if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
		};
		const handleKeyDown = (event: KeyboardEvent) => {
			if (event.key === 'Escape') setOpen(false);
		};

		document.addEventListener('pointerdown', handlePointerDown);
		document.addEventListener('keydown', handleKeyDown);
		return () => {
			document.removeEventListener('pointerdown', handlePointerDown);
			document.removeEventListener('keydown', handleKeyDown);
		};
	}, [open]);

	const selectedLabel = REALMS[selectedRealm]?.label ?? 'Select a realm';

	return (
		<>
			<p className="text-2xl">Select server:</p>
			<div className="flex w-full">
				<div ref={rootRef} className="relative w-full">
					{/*
						Chromium renders a native <select> popup as a separate OS window and
						does not apply the element's colours to <option> on Linux, so under
						Wine/Proton the list could fail to appear or render unreadably. This
						is plain in-page DOM instead. Activation is on pointer press to match
						components/styled/Button, which moved off click for the same reason.
					*/}
					<button
						type="button"
						onPointerDown={event => {
							if (event.button !== 0 || isLoading) return;
							event.preventDefault();
							setOpen(current => !current);
						}}
						onKeyDown={event => {
							if (
								event.key === 'Enter' ||
								event.key === ' ' ||
								event.key === 'ArrowDown'
							) {
								event.preventDefault();
								setOpen(true);
							}
						}}
						disabled={isLoading}
						aria-haspopup="listbox"
						aria-expanded={open}
						className={cls(
							'flex w-full items-center justify-between gap-2 rounded border border-dark bg-dark p-3 text-left text-lg uppercase text-text',
							{ 'opacity-60': isLoading }
						)}
					>
						<span className="truncate">{selectedLabel}</span>
						<ChevronDown
							size={20}
							className={cls('shrink-0 transition-transform', {
								'rotate-180': open
							})}
						/>
					</button>

					{open && (
						<ul
							role="listbox"
							className="absolute left-0 right-0 top-full z-50 mt-1 max-h-60 overflow-y-auto rounded border border-border bg-dark py-1"
						>
							{availableRealmEntries.map(([id, meta]) => (
								<li key={id} role="option" aria-selected={id === selectedRealm}>
									<button
										type="button"
										onPointerDown={event => {
											if (event.button !== 0) return;
											event.preventDefault();
											void onSelect(id);
										}}
										// Keyboard activation arrives as a click with detail 0, the
										// same discrimination components/styled/Button relies on.
										onClick={event => {
											if (event.detail === 0) void onSelect(id);
										}}
										className={cls(
											'w-full px-3 py-2 text-left text-lg uppercase hocus:bg-border/40',
											id === selectedRealm ? 'text-primary' : 'text-text'
										)}
									>
										{meta.label}
									</button>
								</li>
							))}
						</ul>
					)}
				</div>
			</div>
		</>
	);
};

export default RealmSwitch;
