import { useForm } from 'react-hook-form';
import { useEffect } from 'react';

import { PreferencesSchema } from '~common/schemas';
import zodResolver from '~renderer/utils/zodResolver';
import { api } from '~renderer/utils/api';

import TextButton from './styled/TextButton';
import FilePickerInput from './form/FilePickerInput';
import CloseButton from './styled/CloseButton';

type Props = { close: () => void };

const ClientDirDialog = ({ close }: Props) => {
	const { data: pref } = api.preferences.get.useQuery();
	const setPref = api.preferences.set.useMutation();
	const isValidClientDir = api.preferences.isValidClientDir.useQuery(
		pref?.clientDir,
		{ enabled: !!pref?.isPortable }
	);

	const verify = api.updater.verify.useMutation();

	const {
		register,
		handleSubmit,
		watch,
		formState,
		setValue,
		setError,
		reset
	} = useForm({
		defaultValues: { clientDir: pref?.clientDir ?? '' },
		resolver: zodResolver(PreferencesSchema.pick({ clientDir: true }))
	});

	// Form reset
	useEffect(() => {
		pref && reset(pref);
	}, [reset, pref]);

	if (pref?.isPortable) {
		return (
			<form className="dialog">
				<CloseButton close={close} />
				<h2 className="color mb-2 text-xl">Install location</h2>
				<p>
					You are using the portable version of the launcher. Install location
					is determined by the location of the launcher executable.
				</p>
				{!isValidClientDir.isLoading && !isValidClientDir.data && (
					<p>
						<span className="text-secondary">Error: </span>
						WoW.exe not found in current folder. Please close the launcher and
						move it to your WoW Wrath of the Lich King client directory.
					</p>
				)}
			</form>
		);
	}

	return (
		<form
			className="dialog"
			onSubmit={handleSubmit(async ({ clientDir }) => {
				try {
					await setPref.mutateAsync({ clientDir });
					verify.mutateAsync();
					close();
				} catch (e) {
					setError('clientDir', {
						message: e instanceof Error ? e.message : JSON.stringify(e)
					});
				}
			})}
		>
			<CloseButton
				close={() => {
					reset();
					close();
				}}
			/>
			<h2 className="color mb-2 text-xl">Install location</h2>

			<p>Locate your WoW Wrath of the Lich King client directory.</p>
			<div className="flex items-center gap-3">
				<label htmlFor="clientDir">Install directory:</label>
				<FilePickerInput
					{...register('clientDir')}
					title={watch('clientDir') ?? undefined}
					setValue={v =>
						setValue('clientDir', v, {
							shouldTouch: true,
							shouldDirty: true,
							shouldValidate: true
						})
					}
					options={{ properties: ['openDirectory', 'createDirectory'] }}
				/>
			</div>
			{formState.errors.clientDir && (
				<p className="text-sm text-secondary">
					{formState.errors.clientDir.message}
				</p>
			)}

			<TextButton
				type="submit"
				loading={formState.isSubmitting}
				className="text-green self-end"
			>
				Confirm
			</TextButton>
		</form>
	);
};

export default ClientDirDialog;
