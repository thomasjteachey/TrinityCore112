import { type Config } from 'tailwindcss';
import plugin from 'tailwindcss/plugin';

export default {
	content: ['./src/renderer/index.html', './src/renderer/**/*.{js,ts,jsx,tsx}'],
	theme: {
		colors: {
			inherit: 'inherit',
			current: 'currentColor',
			text: '#e7e5e4',
			textDark: '#a8a29e',
			border: '#363230',
			primary: '#facc15',
			secondary: '#f87171',
			dark: '#0c0a09',
			darkBrown: '#140701'
		},
		extend: {
			// fontFamily: {
			// 	fontin: ['fontin'],
			// 	din: ['din']
			// },
			animation: {
				progress: 'progress 2s linear infinite'
			},
			keyframes: {
				progress: {
					'0%': { backgroundPosition: '1rem 0' },
					'100%': { backgroundPosition: '0 0' }
				}
			}
		}
	},
	plugins: [
		require('@tailwindcss/container-queries'),
		plugin(({ addVariant }) => {
			addVariant('hocus', ['&:hover', '&:focus']);
			addVariant('hocus-within', ['&:hover', '&:focus-within']);
			addVariant('hover-row', ['&:hover>div:first-child']);
		})
	]
} satisfies Config;
