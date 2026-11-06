#!/usr/bin/env python3
"""CPU checks for typed private-process weapon diagnostics."""
import unittest
import diagnose_radiance_nailgun as diagnostic


class WeaponDiagnosticTest(unittest.TestCase):
    def test_frozen_dwarf_rejects_cpu_double_read_as_float(self):
        output='FIELD_cpu_prepare_ms=$1 = 0x1200\nSIZE_cpu_prepare_ms=8\n'
        self.assertEqual(diagnostic.resolve_addresses(output,{'cpu_prepare_ms':('field','d')},0x4000),
                         {'cpu_prepare_ms':0x5200})
        with self.assertRaisesRegex(RuntimeError,'type size differs'):
            diagnostic.resolve_addresses(output,{'cpu_prepare_ms':('field','f')},0)

    def test_summary_preserves_shot_proof_and_ignores_unpublished_cpu_profile(self):
        samples=[]
        for i in range(3):
            samples.append(dict(simulation_time=10+i,active_weapon=4,nails=100-i,
                atlas_pixel_revision=3,material_selection_revision=4+i,triangles=277292+i*4,
                same_host_frame=True,cpu_profile_available=i!=1,cpu_prepare_ms=(20,999,100)[i]))
        result=diagnostic.summarize(samples)
        self.assertEqual(result['nails_before']-result['nails_after'],2)
        self.assertEqual(result['atlas_pixel_revisions'],[3])
        self.assertEqual(result['valid_cpu_samples'],2)
        self.assertEqual(result['cpu_prepare_ms'],{'median':60,'max':100})
        self.assertEqual(diagnostic.WEAPONS['lightning'],(8,64,'cells'))


if __name__=='__main__':unittest.main()
