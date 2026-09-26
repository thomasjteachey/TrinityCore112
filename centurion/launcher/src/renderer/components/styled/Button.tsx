import type { ButtonHTMLAttributes, MouseEvent } from 'react';
import cls from 'classnames';
import { type LucideIcon } from 'lucide-react';

import { focusBlur } from '~common/utils';

import IconSpinner from './IconSpinner';

type Props = ButtonHTMLAttributes<HTMLButtonElement> & {
	primary?: boolean;
	loading?: boolean;
	disabled?: boolean;
	icon?: LucideIcon;
};

const Button = ({
        primary,
        loading,
        disabled,
        icon: Icon,
        children,
        className,
        onClick,
        onMouseDown,
        tabIndex,
        ...props
}: Props) => {
        const activate = focusBlur(onClick);
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
                        {...props}
                        onMouseDown={handleMouseDown}
                        onClick={handleClick}
                        tabIndex={!!loading || !!disabled ? -1 : tabIndex}
                        className={cls(
                                'rounded border-2 border-border bg-dark px-5 py-3',
                                className,
                                {
                                        'pointer-events-none': !!disabled || !!loading,
                                        'grayscale': disabled,
                                        'button-primary': primary
                                }
                        )}
                >
                        <span className={cls('select-none', { 'ml-[-12px]': !!loading || !!Icon })}>
                                {loading ? (
                                        <IconSpinner size={23} strokeWidth={1.5} />
                                ) : Icon ? (
                                        <Icon size={23} strokeWidth={1.5} />
                                ) : null}
                                {children}
                        </span>
                </button>
        );
};

export default Button;
