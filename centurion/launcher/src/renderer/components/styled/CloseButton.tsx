import { X } from 'lucide-react';

import TextButton from './TextButton';

const CloseButton = ({ close }: { close: () => void }) => (
	<TextButton
		title="Close"
		icon={X}
		size={16}
		onClick={close}
		className="absolute right-2 top-2 hocus:text-secondary"
	/>
);
export default CloseButton;
