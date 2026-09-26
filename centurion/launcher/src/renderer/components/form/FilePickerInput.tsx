import cls from 'classnames';
import { forwardRef, type HTMLProps } from 'react';
import { AppWindow, FolderOpen } from 'lucide-react';

import { api, type RouterInputs } from '~renderer/utils/api';

import TextButton from '../styled/TextButton';

type Props = HTMLProps<HTMLInputElement> & {
	setValue: (newVal: string) => void;
	options: RouterInputs['general']['filePicker'];
};

const FilePickerInput = forwardRef<HTMLInputElement, Props>(
	({ setValue, options, className, ...props }, ref) => {
		const filePicker = api.general.filePicker.useMutation();
		return (
			<div className="relative flex">
				<input
					ref={ref}
					id={props.name}
					{...props}
					className={cls(
						'border-b border-text bg-inherit p-1 pr-[44px] hocus:border-primary',
						className
					)}
				/>
				<TextButton
					className="absolute right-1 top-0 h-full"
					icon={
						options.properties?.includes('openDirectory')
							? FolderOpen
							: AppWindow
					}
					title="Pick file"
					onClick={async () => {
						const r = await filePicker.mutateAsync(options);
						if (r.canceled) return;
						setValue(r.path[0]);
					}}
				/>
			</div>
		);
	}
);

export default FilePickerInput;
