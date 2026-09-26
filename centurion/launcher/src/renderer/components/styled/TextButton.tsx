import cls from 'classnames';
import { type LucideIcon } from 'lucide-react';
import {
        type ButtonHTMLAttributes,
        type MouseEvent,
        type ReactNode
} from 'react';

import { focusBlur } from '~common/utils';

import IconSpinner from './IconSpinner';

type BaseProps = Omit<
        ButtonHTMLAttributes<HTMLButtonElement>,
        'onClick' | 'type' | 'children'
>;

type Props = BaseProps & {
        active?: boolean;
        loading?: boolean;
        disabled?: boolean;
        size?: number;
        className?: cls.Value;
        style?: React.CSSProperties;
} & (
        | { type: 'submit'; onClick?: never }
        | { type?: never; onClick: () => void }
) &
	(
		| { children: ReactNode; icon?: LucideIcon; title?: never }
		| { children?: never; icon: LucideIcon; title: string }
	);

const TextButton = ({
        title,
        type,
        active,
        loading,
        disabled,
        icon: Icon,
        size,
        onClick,
        onMouseDown,
        className,
        children,
        ...props
}: Props) => {
        const { tabIndex, ...rest } = props;

        const activate = focusBlur(onClick ?? true);
        const triggerActivate = (event: MouseEvent<HTMLButtonElement>) => {
                activate?.(
                        event as Parameters<NonNullable<typeof activate>>[0]
                );
        };

        const handleMouseDown = (event: MouseEvent<HTMLButtonElement>) => {
                onMouseDown?.(event);
                if (!event.defaultPrevented && event.button === 0) {
                        triggerActivate(event);
                }
        };

        const handleClick = (event: MouseEvent<HTMLButtonElement>) => {
                if (event.detail === 0) {
                        triggerActivate(event);
                }
        };

        return (
                <button
                        title={title ?? (typeof children === 'string' ? children : undefined)}
                        type={type ?? 'button'}
                        onMouseDown={handleMouseDown}
                        onClick={handleClick}
                        tabIndex={!!loading || !!disabled ? -1 : tabIndex}
                        className={cls(
                                'flex cursor-pointer items-center gap-2 border-0 p-2',
                                className,
                                {
                                        'drop-shadow-[0px_0px_10px_white]': active && !loading && !disabled,
                                        'text-gray pointer-events-none': !!loading || !!disabled,
                                        'hocus': !loading && !disabled
                                }
                        )}
                        {...rest}
                >
                        {loading ? (
                                <IconSpinner size={size ?? 24} strokeWidth={1.5} />
                        ) : (
                                Icon && <Icon size={size} />
                        )}
                        {children && (
                                <span className="cursor-pointer select-none tracking-wide text-inherit [font-size:_inherit]">
                                        {children}
                                </span>
                        )}
                </button>
        );
};

export default TextButton;
