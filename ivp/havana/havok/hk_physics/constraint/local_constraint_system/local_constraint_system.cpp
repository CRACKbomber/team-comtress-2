#include <hk_physics/physics.h>
#include <ivp_environment.hxx>
#include <hk_physics/simunit/psi_info.h>
#include <hk_physics/constraint/constraint.h>
#include <hk_physics/constraint/local_constraint_system/local_constraint_system.h>

hk_Local_Constraint_System::hk_Local_Constraint_System( hk_Environment *env, hk_Local_Constraint_System_BP* bp )
  : hk_Link_EF(env)
  , m_n_iterations(0)
  , m_size_of_all_vmq_storages(0)
  , m_minErrorTicks(0)
  , m_errorCount(0)
  , m_penetrationCount(0)
  , m_errorTolerance(0)
  , m_is_active(0)
  , m_errorThisTick(0)
  , m_needsSort(0)
  , m_client_data(0)
{
	m_environment = env;
	m_size_of_all_vmq_storages = 0;

    // pull in number of iterations and error tolerance from blueprint
    m_n_iterations = bp->m_n_iterations;
    m_errorTolerance = bp->m_errorTolerance;
}

hk_Local_Constraint_System::~hk_Local_Constraint_System()
{
	while ( m_constraints.length() )
	{
		delete m_constraints.get_element( m_constraints.start());
	}

	//XXX hack for havok
//	if(m_environment)
//	{
//		m_environment->get_sim_mgr()->remove_link_effector( this );
//	}

    if( m_is_active )
    {
        deactivate();
    }
}

void hk_Local_Constraint_System::get_constraints_in_system(hk_Array<hk_Constraint*>& constraints_out)
{
  for (hk_Array<hk_Constraint*>::iterator i = m_constraints.start();
    m_constraints.is_valid(i);
    i = m_constraints.next(i))
  {
    constraints_out.add_element(m_constraints.get_element(i));
  }
}

void hk_Local_Constraint_System::entity_deletion_event(hk_Entity *entity)
{
	HK_BREAK;  // XXX fix me
}


void hk_Local_Constraint_System::constraint_deletion_event( hk_Constraint *constraint )
{
	m_constraints.search_and_remove_element_sorted(constraint);
	if ( m_constraints.length() == 0){
		delete this;
		return;
	}else{
		recalc_storage_size();
	}
}


void hk_Local_Constraint_System::recalc_storage_size()
{
	m_size_of_all_vmq_storages = 0;
	for ( hk_Array<hk_Constraint *>::iterator i = m_constraints.start();
			m_constraints.is_valid(i);
			i = m_constraints.next( i ) )
	{
		m_size_of_all_vmq_storages += m_constraints.get_element(i)->get_vmq_storage_size();
	}
}


void hk_Local_Constraint_System::add_constraint( hk_Constraint * constraint, int storage_size)
{
	m_constraints.add_element( constraint );
	
	int i = 1;
	do {
		hk_Rigid_Body *b = constraint->get_rigid_body(i);
		if ( m_bodies.index_of( b ) <0){
			m_bodies.add_element(b );
		}
	} while (--i>=0);

	m_size_of_all_vmq_storages += storage_size;	
}

void hk_Local_Constraint_System::activate()
{
  if (!m_is_active && m_bodies.length())
  {
	// notify enivronment observer this controller is active.
	m_environment->get_controller_manager()->announce_controller_to_environment(this);
	this->m_is_active = true;
  }
}

void hk_Local_Constraint_System::deactivate()
{
    if ( m_is_active && (this->actuator_controlled_cores.len() != 0) ){
        m_environment->get_controller_manager()->remove_controller_from_environment(this, IVP_FALSE);
        this->m_is_active = false;
    }
}

void hk_Local_Constraint_System::deactivate_silently()
{
    if ( m_is_active && (this->actuator_controlled_cores.len() != 0) ){
        m_environment->get_controller_manager()->remove_controller_from_environment(this, IVP_TRUE);
        this->m_is_active = false;
    }
}

void hk_Local_Constraint_System::write_to_blueprint( hk_Local_Constraint_System_BP *bpOut )
{
    // damp/tau values extracted with ghidra
    bpOut->m_damp = 0x3f800000;
    bpOut->m_tau = 0x3f800000;
    bpOut->m_n_iterations = m_n_iterations;
    bpOut->m_minErrorTicks = m_minErrorTicks;
    bpOut->m_errorTolerance = m_errorTolerance;
    bpOut->m_active = m_is_active;
}

void hk_Local_Constraint_System::solve_penetration(IVP_Real_Object* pivp0, IVP_Real_Object* pivp1)
{
	if (m_penetrationCount >= 4)
		return;

	for (hk_Array<hk_Entity*>::iterator i = m_bodies.start();
		(m_bodies.is_valid(i) && pivp0 != m_bodies.get_element(i));
		i = m_bodies.next(i))
	{
		m_penetrationPairs[m_penetrationCount].obj0 = i;
	}

	for (hk_Array<hk_Entity*>::iterator i = m_bodies.start();
		(m_bodies.is_valid(i) && pivp1 != m_bodies.get_element(i));
		i = m_bodies.next(i))
	{
		m_penetrationPairs[m_penetrationCount].obj1 = i;
	}

	if (m_penetrationPairs[m_penetrationCount].obj0 > -1 && m_penetrationPairs[m_penetrationCount].obj1 > -1)
		m_penetrationCount++;
}

void hk_Local_Constraint_System::get_effected_entities(hk_Array<hk_Entity*> &ent_out)
{
	for ( hk_Array<hk_Entity*>::iterator i = m_bodies.start();
			m_bodies.is_valid(i);
			i = m_bodies.next(i))
	{
		ent_out.add_element( (hk_Entity*)m_bodies.get_element(i));
	}
}
		
	//virtual hk_real get_minimum_simulation_frequency(hk_Array<hk_Entity> *);

hk_real hk_Local_Constraint_System::get_epsilon()
{
	return 0.2f;
}

void hk_Local_Constraint_System::report_square_error(float errSq)
{
  m_errorThisTick = ((m_errorTolerance * m_errorTolerance) < errSq);
}

void hk_Local_Constraint_System::apply_effector_PSI(	hk_PSI_Info& pi, hk_Array<hk_Entity*>* )
{
	const int buffer_size = 150000;
	const int max_constraints = 1000;
	void *vmq_buffers[ max_constraints ];
	char buffer[buffer_size];
	HK_CHECK( m_size_of_all_vmq_storages < buffer_size );

	//hk_real taus[] = { 0.2f, 0.2f, 0.0f, 0.6f, 0.4f, 0.0f };
	hk_real taus[] = { 1.0f, 1.0f, 0.0f, 0.6f, 0.4f, 0.0f };
	hk_real damps[] = { 1.0f, 1.0f, 0.8f, 0.8f, 0.0f };
	//first do the setup
	{
		char *p_buffer = &buffer[0];
		for ( int i = 0; i < m_constraints.length(); i++ ){
//		for ( int i = m_constraints.length()-1; i>=0; i-- ){
			vmq_buffers[i] = (void *)p_buffer;
			int b_size = m_constraints.element_at(i)->setup_and_step_constraint( pi, (void *) p_buffer,1.0f, 1.0f);
			p_buffer += b_size;
		}
	}
	
//	return;
	// do the steps
	for (int x = 0; x< 10; x++)	{ 
		if ( taus[x]==0.0f ) break;
		for ( int i = m_constraints.length()-2; i >=0 ; i-- ){
			m_constraints.element_at(i)->step_constraint( pi, (void *)vmq_buffers[i],taus[x], damps[x]);
		}
		for ( int j = 1; j < m_constraints.length(); j++ ){
			m_constraints.element_at(j)->step_constraint( pi, (void *)vmq_buffers[j],taus[x], damps[x]);
		}
	}
}

const char* hk_Local_Constraint_System::get_controller_name(void)
{
    return "sys:constraint";
}

hk_effector_priority hk_Local_Constraint_System::get_effector_priority(void)
{
    return HK_PRIORITY_LOCAL_CONSTRAINT_SYSTEM;
}

void hk_Local_Constraint_System::apply_effector_collision(hk_PSI_Info&, hk_Array<hk_Entity*>*)
{
}
