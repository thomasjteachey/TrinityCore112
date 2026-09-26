import { useState, type ReactElement } from 'react';
import cls from 'classnames';

import { type LauncherUpdaterStatus, type UpdaterStatus } from '~main/types';
import { api } from '~renderer/utils/api';

import Button from './styled/Button';
import DialogButton from './styled/DialogButton';
import ClientDirDialog from './ClientDirDialog';

const LaunchPanel = () => {
	const [status, setStatus] = useState<UpdaterStatus>({ state: 'verifying' });
	api.updater.observe.useSubscription(undefined, {
		onData: data => setStatus(data)
	});
	const [launcherUpdate, setLauncherUpdate] =
		useState<LauncherUpdaterStatus>({ state: 'idle' });
	const [launchError, setLaunchError] = useState<string>();
	api.launcherUpdater.observe.useSubscription(undefined, {
		onData: data => setLauncherUpdate(data)
	});

	const verify = api.updater.verify.useMutation();
	const update = api.updater.update.useMutation();
	const start = api.launcher.start.useMutation({
		onMutate: () => setLaunchError(undefined),
		onError: error => setLaunchError(error.message)
	});
	const restartLauncher = api.launcherUpdater.restartAndInstall.useMutation();
	const openLink = api.general.openLink.useMutation();

	const props: Record<
		UpdaterStatus['state'],
		{ button: ReactElement; helperText?: ReactElement }
	> = {
		needsValidation: {
			button: <Button onClick={() => verify.mutateAsync()}>Verify</Button>,
			helperText: (
				<div className="-mb-2">
					<p>New changes detected</p>
					<p className="text-xs text-textDark">Please verify your game data</p>
				</div>
			)
		},
		verifying: { button: <Button disabled>Verifying</Button> },
		serverUnreachable: {
			button: <Button onClick={() => verify.mutateAsync()}>Retry</Button>,
			helperText: (
				<div className="-mb-2">
					<p>
						<span className="text-secondary">Error: </span> Failed to reach
						update server
					</p>
					<p className="text-xs text-textDark">Please try again later</p>
				</div>
			)
		},
		noClient: {
			button: (
				<DialogButton
					clickAway
					dialog={close => <ClientDirDialog close={close} />}
				>
					{open => (
						<Button primary onClick={open}>
							Locate client
						</Button>
					)}
				</DialogButton>
			),
			helperText: (
				<div className="-mb-2">
					<p>Client location was not yet selected</p>
					<p className="text-xs text-textDark">
						Please select your World of Warcraft 3.3.5 location
					</p>
				</div>
			)
		},
		updateAvailable: {
			button: <Button onClick={() => update.mutateAsync()}>Update</Button>,
			helperText: (
				<div className="-mb-2">
					<p>Update available!</p>
					<p className="text-xs text-textDark">
						With total download size{' '}
						<span className="text-xs text-text">{status.message}</span>
					</p>
				</div>
			)
		},
		updating: { button: <Button disabled>Updating</Button> },
		upToDate: {
			button: (
				<Button
					primary
					loading={start.isLoading}
					onClick={() => {
						void start.mutateAsync().catch(() => undefined);
					}}
				>
					Play
				</Button>
			),
			helperText: (
				<div className="-mb-2">
					<p>Everything up to date!</p>
				</div>
			)
		},
		failed: {
			button: <Button onClick={() => verify.mutateAsync()}>Retry</Button>,
			helperText: (
				<div className="-mb-2">
					<p>
						<span className="text-secondary">Error: </span>
						{status.message}
					</p>
					<p className="text-xs text-textDark">
						Verify your game data by clicking Retry.
					</p>
				</div>
			)
		}
	};

	const base = props[status.state];
	let button = base.button;
	let helperText = base.helperText;
	let progress = status.progress;

	if (launcherUpdate.state === 'downloading') {
		button = <Button disabled>Updating launcher</Button>;
		helperText = <p className="-mb-2 text-xs">{launcherUpdate.message}</p>;
		progress = launcherUpdate.progress ?? -1;
	} else if (launcherUpdate.state === 'ready') {
		button = (
			<Button
				primary
				disabled={restartLauncher.isLoading}
				onClick={() => restartLauncher.mutateAsync()}
			>
				Restart to update
			</Button>
		);
		helperText = (
			<div className="-mb-2">
				<p>Launcher {launcherUpdate.version} is ready.</p>
				<p className="text-xs text-textDark">Restart to install the update.</p>
			</div>
		);
		progress = 1;
	} else if (launcherUpdate.state === 'installing') {
		button = <Button disabled>Restarting</Button>;
		helperText = <p className="-mb-2 text-xs">{launcherUpdate.message}</p>;
		progress = -1;
	} else if (launcherUpdate.state === 'manualUpdate') {
		const downloadUrl = launcherUpdate.downloadUrl;
		helperText = (
			<div className="-mb-2">
				<p>Launcher {launcherUpdate.version} is available.</p>
				<p className="text-xs text-textDark">
					{launcherUpdate.message}{' '}
					{downloadUrl && (
						<button
							type="button"
							className="cursor-pointer border-0 text-primary underline"
							onClick={() => openLink.mutate(downloadUrl)}
						>
							Download the update ZIP
						</button>
					)}
				</p>
			</div>
		);
	}

	if (launchError) {
		helperText = (
			<div className="-mb-2">
				<p>
					<span className="text-secondary">Launch failed: </span>
					{launchError}
				</p>
			</div>
		);
	}

	return (
		<div className="flex gap-3">
			<div className="flex flex-grow select-none flex-col justify-end gap-3">
				{helperText ??
					(status.message && <p className="-mb-2 text-xs">{status.message}</p>)}
				{launcherUpdate.state === 'error' && (
					<p className="text-xs text-secondary">{launcherUpdate.message}</p>
				)}
				<div className="loading-wrapper">
					{progress !== undefined && (
						<div
							className={cls('loading', {
								'loading-unknown': progress === -1
							})}
							style={
								progress !== -1
									? {
											clipPath: `inset(0 ${
												100 - Math.ceil(Math.abs(progress) * 100)
											}% 0 0)`
									  }
									: undefined
							}
						/>
					)}
				</div>
			</div>
			{button}
		</div>
	);
};

export default LaunchPanel;
